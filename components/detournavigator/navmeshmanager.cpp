#include "navmeshmanager.hpp"
#include "debug.hpp"
#include "exceptions.hpp"
#include "gettilespositions.hpp"
#include "makenavmesh.hpp"
#include "navmeshcacheitem.hpp"
#include "settings.hpp"
#include "waitconditiontype.hpp"

#include <components/debug/debuglog.hpp>

#include <DetourNavMesh.h>

#include <iterator>

namespace
{
    using DetourNavigator::ChangeType;

    ChangeType addChangeType(const ChangeType current, const ChangeType add)
    {
        return current == add ? current : ChangeType::mixed;
    }

    /// Safely reset shared_ptr with definite underlying object destrutor call.
    /// Assuming there is another thread holding copy of this shared_ptr or weak_ptr to this shared_ptr.
    template <class T>
    bool resetIfUnique(std::shared_ptr<T>& ptr)
    {
        const std::weak_ptr<T> weak(ptr);
        ptr.reset();
        if (auto shared = weak.lock())
        {
            ptr = std::move(shared);
            return false;
        }
        return true;
    }
}

namespace DetourNavigator
{
    NavMeshManager::NavMeshManager(const Settings& settings)
        : mSettings(settings)
        , mRecastMeshManager(settings)
        , mOffMeshConnectionsManager(settings)
        , mAsyncNavMeshUpdater(settings, mRecastMeshManager, mOffMeshConnectionsManager)
    {}

    bool NavMeshManager::addObject(const ObjectId id, const CollisionShape& shape, const btTransform& transform,
                                   const AreaType areaType)
    {
        const btCollisionShape& collisionShape = shape.getShape();
        if (!mRecastMeshManager.addObject(id, shape, transform, areaType))
            return false;
        addChangedTiles(collisionShape, transform, ChangeType::add);
        return true;
    }

    bool NavMeshManager::updateObject(const ObjectId id, const CollisionShape& shape, const btTransform& transform,
                                      const AreaType areaType)
    {
        return mRecastMeshManager.updateObject(id, shape, transform, areaType,
            [&] (const TilePosition& tile) { addChangedTile(tile, ChangeType::update); });
    }

    bool NavMeshManager::removeObject(const ObjectId id)
    {
        const auto object = mRecastMeshManager.removeObject(id);
        if (!object)
            return false;
        addChangedTiles(object->mShape, object->mTransform, ChangeType::remove);
        return true;
    }

    bool NavMeshManager::addWater(const osg::Vec2i& cellPosition, const int cellSize, const osg::Vec3f& shift)
    {
        if (!mRecastMeshManager.addWater(cellPosition, cellSize, shift))
            return false;
        addChangedTiles(cellSize, shift, ChangeType::add);
        return true;
    }

    bool NavMeshManager::removeWater(const osg::Vec2i& cellPosition)
    {
        const auto water = mRecastMeshManager.removeWater(cellPosition);
        if (!water)
            return false;
        addChangedTiles(water->mSize, water->mShift, ChangeType::remove);
        return true;
    }

    bool NavMeshManager::addHeightfield(const osg::Vec2i& cellPosition, int cellSize, const osg::Vec3f& shift,
        const HeightfieldShape& shape)
    {
        if (!mRecastMeshManager.addHeightfield(cellPosition, cellSize, shift, shape))
            return false;
        addChangedTiles(cellSize, shift, ChangeType::add);
        return true;
    }

    bool NavMeshManager::removeHeightfield(const osg::Vec2i& cellPosition)
    {
        const auto heightfield = mRecastMeshManager.removeHeightfield(cellPosition);
        if (!heightfield)
            return false;
        addChangedTiles(heightfield->mSize, heightfield->mShift, ChangeType::remove);
        return true;
    }

    void NavMeshManager::addAgent(const osg::Vec3f& agentHalfExtents)
    {
        auto cached = mCache.find(agentHalfExtents);
        if (cached != mCache.end())
            return;
        mCache.insert(std::make_pair(agentHalfExtents,
            std::make_shared<GuardedNavMeshCacheItem>(makeEmptyNavMesh(mSettings), ++mGenerationCounter)));
        Log(Debug::Debug) << "cache add for agent=" << agentHalfExtents;
    }

    bool NavMeshManager::reset(const osg::Vec3f& agentHalfExtents)
    {
        const auto it = mCache.find(agentHalfExtents);
        if (it == mCache.end())
            return true;
        if (!resetIfUnique(it->second))
            return false;
        mCache.erase(agentHalfExtents);
        mChangedTiles.erase(agentHalfExtents);
        mPlayerTile.erase(agentHalfExtents);
        mLastRecastMeshManagerRevision.erase(agentHalfExtents);
        return true;
    }

    void NavMeshManager::addOffMeshConnection(const ObjectId id, const osg::Vec3f& start, const osg::Vec3f& end, const AreaType areaType)
    {
        mOffMeshConnectionsManager.add(id, OffMeshConnection {start, end, areaType});

        const auto startTilePosition = getTilePosition(mSettings, start);
        const auto endTilePosition = getTilePosition(mSettings, end);

        addChangedTile(startTilePosition, ChangeType::add);

        if (startTilePosition != endTilePosition)
            addChangedTile(endTilePosition, ChangeType::add);
    }

    void NavMeshManager::removeOffMeshConnections(const ObjectId id)
    {
        const auto changedTiles = mOffMeshConnectionsManager.remove(id);
        for (const auto& tile : changedTiles)
            addChangedTile(tile, ChangeType::update);
    }

    void NavMeshManager::update(const osg::Vec3f& playerPosition, const osg::Vec3f& agentHalfExtents)
    {
        const auto playerTile = getTilePosition(mSettings, toNavMeshCoordinates(mSettings, playerPosition));
        auto& lastRevision = mLastRecastMeshManagerRevision[agentHalfExtents];
        auto lastPlayerTile = mPlayerTile.find(agentHalfExtents);
        if (lastRevision == mRecastMeshManager.getRevision() && lastPlayerTile != mPlayerTile.end()
                && lastPlayerTile->second == playerTile)
            return;
        lastRevision = mRecastMeshManager.getRevision();
        if (lastPlayerTile == mPlayerTile.end())
            lastPlayerTile = mPlayerTile.insert(std::make_pair(agentHalfExtents, playerTile)).first;
        else
            lastPlayerTile->second = playerTile;
        std::map<TilePosition, ChangeType> tilesToPost;
        const auto cached = getCached(agentHalfExtents);
        if (!cached)
        {
            std::ostringstream stream;
            stream << "Agent with half extents is not found: " << agentHalfExtents;
            throw InvalidArgument(stream.str());
        }
        const auto changedTiles = mChangedTiles.find(agentHalfExtents);
        {
            const auto locked = cached->lockConst();
            const auto& navMesh = locked->getImpl();
            if (changedTiles != mChangedTiles.end())
            {
                for (const auto& tile : changedTiles->second)
                    if (navMesh.getTileAt(tile.first.x(), tile.first.y(), 0))
                    {
                        auto tileToPost = tilesToPost.find(tile.first);
                        if (tileToPost == tilesToPost.end())
                            tilesToPost.insert(tile);
                        else
                            tileToPost->second = addChangeType(tileToPost->second, tile.second);
                    }
            }
            const auto maxTiles = std::min(mSettings.mMaxTilesNumber, navMesh.getParams()->maxTiles);
            mRecastMeshManager.forEachTile([&] (const TilePosition& tile, CachedRecastMeshManager& recastMeshManager)
            {
                if (tilesToPost.count(tile))
                    return;
                const auto shouldAdd = shouldAddTile(tile, playerTile, maxTiles);
                const auto presentInNavMesh = bool(navMesh.getTileAt(tile.x(), tile.y(), 0));
                if (shouldAdd && !presentInNavMesh)
                    tilesToPost.insert(std::make_pair(tile, ChangeType::add));
                else if (!shouldAdd && presentInNavMesh)
                    tilesToPost.insert(std::make_pair(tile, ChangeType::mixed));
                else
                    recastMeshManager.reportNavMeshChange(recastMeshManager.getVersion(), Version {0, 0});
            });
        }
        mAsyncNavMeshUpdater.post(agentHalfExtents, cached, playerTile, tilesToPost);
        if (changedTiles != mChangedTiles.end())
            changedTiles->second.clear();
        Log(Debug::Debug) << "Cache update posted for agent=" << agentHalfExtents <<
            " playerTile=" << lastPlayerTile->second <<
            " recastMeshManagerRevision=" << lastRevision;
    }

    void NavMeshManager::wait(Loading::Listener& listener, WaitConditionType waitConditionType)
    {
        mAsyncNavMeshUpdater.wait(listener, waitConditionType);
    }

    SharedNavMeshCacheItem NavMeshManager::getNavMesh(const osg::Vec3f& agentHalfExtents) const
    {
        return getCached(agentHalfExtents);
    }

    std::map<osg::Vec3f, SharedNavMeshCacheItem> NavMeshManager::getNavMeshes() const
    {
        return mCache;
    }

    void NavMeshManager::reportStats(unsigned int frameNumber, osg::Stats& stats) const
    {
        mAsyncNavMeshUpdater.reportStats(frameNumber, stats);
    }

    RecastMeshTiles NavMeshManager::getRecastMeshTiles()
    {
        std::vector<TilePosition> tiles;
        mRecastMeshManager.forEachTile(
            [&tiles] (const TilePosition& tile, const CachedRecastMeshManager&) { tiles.push_back(tile); });
        RecastMeshTiles result;
        std::transform(tiles.begin(), tiles.end(), std::inserter(result, result.end()),
            [this] (const TilePosition& tile) { return std::make_pair(tile, mRecastMeshManager.getMesh(tile)); });
        return result;
    }

    void NavMeshManager::addChangedTiles(const btCollisionShape& shape, const btTransform& transform,
            const ChangeType changeType)
    {
        getTilesPositions(shape, transform, mSettings,
            [&] (const TilePosition& v) { addChangedTile(v, changeType); });
    }

    void NavMeshManager::addChangedTiles(const int cellSize, const osg::Vec3f& shift,
            const ChangeType changeType)
    {
        if (cellSize == std::numeric_limits<int>::max())
            return;

        getTilesPositions(cellSize, shift, mSettings,
            [&] (const TilePosition& v) { addChangedTile(v, changeType); });
    }

    void NavMeshManager::addChangedTile(const TilePosition& tilePosition, const ChangeType changeType)
    {
        for (const auto& cached : mCache)
        {
            auto& tiles = mChangedTiles[cached.first];
            auto tile = tiles.find(tilePosition);
            if (tile == tiles.end())
                tiles.insert(std::make_pair(tilePosition, changeType));
            else
                tile->second = addChangeType(tile->second, changeType);
        }
    }

    SharedNavMeshCacheItem NavMeshManager::getCached(const osg::Vec3f& agentHalfExtents) const
    {
        const auto cached = mCache.find(agentHalfExtents);
        if (cached != mCache.end())
            return cached->second;
        return SharedNavMeshCacheItem();
    }
}
