#include "luamanagerimp.hpp"

#include <components/debug/debuglog.hpp>

#include <components/esm/esmreader.hpp>
#include <components/esm/esmwriter.hpp>
#include <components/esm/luascripts.hpp>

#include <components/lua/utilpackage.hpp>
#include <components/lua/omwscriptsparser.hpp>

#include "../mwbase/windowmanager.hpp"

#include "../mwworld/class.hpp"
#include "../mwworld/ptr.hpp"

#include "luabindings.hpp"
#include "userdataserializer.hpp"

namespace MWLua
{

    LuaManager::LuaManager(const VFS::Manager* vfs, const std::vector<std::string>& scriptLists) : mLua(vfs)
    {
        Log(Debug::Info) << "Lua version: " << LuaUtil::getLuaVersion();
        mGlobalScriptList = LuaUtil::parseOMWScriptsFiles(vfs, scriptLists);

        mGlobalSerializer = createUserdataSerializer(false, mWorldView.getObjectRegistry());
        mLocalSerializer = createUserdataSerializer(true, mWorldView.getObjectRegistry());
        mGlobalLoader = createUserdataSerializer(false, mWorldView.getObjectRegistry(), &mContentFileMapping);
        mLocalLoader = createUserdataSerializer(true, mWorldView.getObjectRegistry(), &mContentFileMapping);

        mGlobalScripts.setSerializer(mGlobalSerializer.get());
    }

    void LuaManager::init()
    {
        Context context;
        context.mIsGlobal = true;
        context.mLuaManager = this;
        context.mLua = &mLua;
        context.mWorldView = &mWorldView;
        context.mLocalEventQueue = &mLocalEvents;
        context.mGlobalEventQueue = &mGlobalEvents;
        context.mSerializer = mGlobalSerializer.get();

        Context localContext = context;
        localContext.mIsGlobal = false;
        localContext.mSerializer = mLocalSerializer.get();

        initObjectBindingsForGlobalScripts(context);
        initCellBindingsForGlobalScripts(context);
        initObjectBindingsForLocalScripts(localContext);
        initCellBindingsForLocalScripts(localContext);
        LocalScripts::initializeSelfPackage(localContext);

        mLua.addCommonPackage("openmw.async", getAsyncPackageInitializer(context));
        mLua.addCommonPackage("openmw.util", LuaUtil::initUtilPackage(mLua.sol()));
        mLua.addCommonPackage("openmw.core", initCorePackage(context));
        mLua.addCommonPackage("openmw.query", initQueryPackage(context));
        mGlobalScripts.addPackage("openmw.world", initWorldPackage(context));
        mGlobalScripts.addPackage("openmw.settings", initGlobalSettingsPackage(context));
        mCameraPackage = initCameraPackage(localContext);
        mUserInterfacePackage = initUserInterfacePackage(localContext);
        mInputPackage = initInputPackage(localContext);
        mNearbyPackage = initNearbyPackage(localContext);
        mLocalSettingsPackage = initLocalSettingsPackage(localContext);
        mPlayerSettingsPackage = initPlayerSettingsPackage(localContext);

        mInputEvents.clear();
        for (const std::string& path : mGlobalScriptList)
            if (mGlobalScripts.addNewScript(path))
                Log(Debug::Info) << "Global script started: " << path;
        mInitialized = true;
    }

    void LuaManager::update(bool paused, float dt)
    {
        ObjectRegistry* objectRegistry = mWorldView.getObjectRegistry();

        if (!mPlayer.isEmpty())
        {
            MWWorld::Ptr newPlayerPtr = MWBase::Environment::get().getWorld()->getPlayerPtr();
            if (!(getId(mPlayer) == getId(newPlayerPtr)))
                throw std::logic_error("Player Refnum was changed unexpectedly");
            if (!mPlayer.isInCell() || !newPlayerPtr.isInCell() || mPlayer.getCell() != newPlayerPtr.getCell())
            {
                mPlayer = newPlayerPtr;  // player was moved to another cell, update ptr in registry
                objectRegistry->registerPtr(mPlayer);
            }
        }
        mWorldView.update();

        if (paused)
        {
            mInputEvents.clear();
            return;
        }

        std::vector<GlobalEvent> globalEvents = std::move(mGlobalEvents);
        std::vector<LocalEvent> localEvents = std::move(mLocalEvents);
        mGlobalEvents = std::vector<GlobalEvent>();
        mLocalEvents = std::vector<LocalEvent>();

        {  // Update time and process timers
            double seconds = mWorldView.getGameTimeInSeconds() + dt;
            mWorldView.setGameTimeInSeconds(seconds);
            double hours = mWorldView.getGameTimeInHours();

            mGlobalScripts.processTimers(seconds, hours);
            for (LocalScripts* scripts : mActiveLocalScripts)
                scripts->processTimers(seconds, hours);
        }

        // Receive events
        for (GlobalEvent& e : globalEvents)
            mGlobalScripts.receiveEvent(e.mEventName, e.mEventData);
        for (LocalEvent& e : localEvents)
        {
            LObject obj(e.mDest, objectRegistry);
            LocalScripts* scripts = obj.isValid() ? obj.ptr().getRefData().getLuaScripts() : nullptr;
            if (scripts)
                scripts->receiveEvent(e.mEventName, e.mEventData);
            else
                Log(Debug::Debug) << "Ignored event " << e.mEventName << " to L" << idToString(e.mDest)
                                  << ". Object not found or has no attached scripts";
        }

        // Engine handlers in local scripts
        PlayerScripts* playerScripts = dynamic_cast<PlayerScripts*>(mPlayer.getRefData().getLuaScripts());
        if (playerScripts)
        {
            for (const auto& event : mInputEvents)
                playerScripts->processInputEvent(event);
        }
        mInputEvents.clear();

        for (const LocalEngineEvent& e : mLocalEngineEvents)
        {
            LObject obj(e.mDest, objectRegistry);
            if (!obj.isValid())
            {
                Log(Debug::Verbose) << "Can not call engine handlers: object" << idToString(e.mDest) << " is not found";
                continue;
            }
            LocalScripts* scripts = obj.ptr().getRefData().getLuaScripts();
            if (scripts)
                scripts->receiveEngineEvent(e.mEvent, objectRegistry);
        }
        mLocalEngineEvents.clear();

        for (LocalScripts* scripts : mActiveLocalScripts)
            scripts->update(dt);

        // Engine handlers in global scripts
        if (mPlayerChanged)
        {
            mPlayerChanged = false;
            mGlobalScripts.playerAdded(GObject(getId(mPlayer), objectRegistry));
        }

        for (ObjectId id : mActorAddedEvents)
            mGlobalScripts.actorActive(GObject(id, objectRegistry));
        mActorAddedEvents.clear();

        mGlobalScripts.update(dt);
    }

    void LuaManager::applyQueuedChanges()
    {
        MWBase::WindowManager* windowManager = MWBase::Environment::get().getWindowManager();
        for (const std::string& message : mUIMessages)
            windowManager->messageBox(message);
        mUIMessages.clear();

        for (std::unique_ptr<Action>& action : mActionQueue)
            action->apply(mWorldView);
        mActionQueue.clear();
        
        if (mTeleportPlayerAction)
            mTeleportPlayerAction->apply(mWorldView);
        mTeleportPlayerAction.reset();
    }

    void LuaManager::clear()
    {
        mActiveLocalScripts.clear();
        mLocalEvents.clear();
        mGlobalEvents.clear();
        mInputEvents.clear();
        mActorAddedEvents.clear();
        mLocalEngineEvents.clear();
        mPlayerChanged = false;
        mWorldView.clear();
        if (!mPlayer.isEmpty())
        {
            mPlayer.getCellRef().unsetRefNum();
            mPlayer.getRefData().setLuaScripts(nullptr);
            mPlayer = MWWorld::Ptr();
        }
    }

    void LuaManager::setupPlayer(const MWWorld::Ptr& ptr)
    {
        if (!mInitialized)
            return;
        if (!mPlayer.isEmpty())
            throw std::logic_error("Player is initialized twice");
        mWorldView.objectAddedToScene(ptr);
        mPlayer = ptr;
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
            localScripts = createLocalScripts(ptr);
        mActiveLocalScripts.insert(localScripts);
        mLocalEngineEvents.push_back({getId(ptr), LocalScripts::OnActive{}});
        mPlayerChanged = true;
    }

    void LuaManager::objectAddedToScene(const MWWorld::Ptr& ptr)
    {
        mWorldView.objectAddedToScene(ptr);  // assigns generated RefNum if it is not set yet.

        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (localScripts)
        {
            mActiveLocalScripts.insert(localScripts);
            mLocalEngineEvents.push_back({getId(ptr), LocalScripts::OnActive{}});
        }

        if (ptr.getClass().isActor() && ptr != mPlayer)
            mActorAddedEvents.push_back(getId(ptr));
    }

    void LuaManager::objectRemovedFromScene(const MWWorld::Ptr& ptr)
    {
        mWorldView.objectRemovedFromScene(ptr);
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (localScripts)
        {
            mActiveLocalScripts.erase(localScripts);
            if (!mWorldView.getObjectRegistry()->getPtr(getId(ptr), true).isEmpty())
                mLocalEngineEvents.push_back({getId(ptr), LocalScripts::OnInactive{}});
        }
    }

    void LuaManager::registerObject(const MWWorld::Ptr& ptr)
    {
        mWorldView.getObjectRegistry()->registerPtr(ptr);
    }

    void LuaManager::deregisterObject(const MWWorld::Ptr& ptr)
    {
        mWorldView.getObjectRegistry()->deregisterPtr(ptr);
    }

    void LuaManager::appliedToObject(const MWWorld::Ptr& toPtr, std::string_view recordId, const MWWorld::Ptr& fromPtr)
    {
        mLocalEngineEvents.push_back({getId(toPtr), LocalScripts::OnConsume{std::string(recordId)}});
    }

    MWBase::LuaManager::ActorControls* LuaManager::getActorControls(const MWWorld::Ptr& ptr) const
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
            return nullptr;
        return localScripts->getActorControls();
    }

    void LuaManager::addLocalScript(const MWWorld::Ptr& ptr, const std::string& scriptPath)
    {
        LocalScripts* localScripts = ptr.getRefData().getLuaScripts();
        if (!localScripts)
        {
            localScripts = createLocalScripts(ptr);
            if (ptr.isInCell() && MWBase::Environment::get().getWorld()->isCellActive(ptr.getCell()))
                mActiveLocalScripts.insert(localScripts);
        }
        localScripts->addNewScript(scriptPath);
    }

    LocalScripts* LuaManager::createLocalScripts(const MWWorld::Ptr& ptr)
    {
        assert(mInitialized);
        std::shared_ptr<LocalScripts> scripts;
        // When loading a game, it can be called before LuaManager::setPlayer,
        // so we can't just check ptr == mPlayer here.
        if (ptr.getCellRef().getRefIdRef() == "player")
        {
            scripts = std::make_shared<PlayerScripts>(&mLua, LObject(getId(ptr), mWorldView.getObjectRegistry()));
            scripts->addPackage("openmw.ui", mUserInterfacePackage);
            scripts->addPackage("openmw.camera", mCameraPackage);
            scripts->addPackage("openmw.input", mInputPackage);
            scripts->addPackage("openmw.settings", mPlayerSettingsPackage);
        }
        else
        {
            scripts = std::make_shared<LocalScripts>(&mLua, LObject(getId(ptr), mWorldView.getObjectRegistry()));
            scripts->addPackage("openmw.settings", mLocalSettingsPackage);
        }
        scripts->addPackage("openmw.nearby", mNearbyPackage);
        scripts->setSerializer(mLocalSerializer.get());

        MWWorld::RefData& refData = ptr.getRefData();
        refData.setLuaScripts(std::move(scripts));
        return refData.getLuaScripts();
    }

    void LuaManager::write(ESM::ESMWriter& writer, Loading::Listener& progress)
    {
        writer.startRecord(ESM::REC_LUAM);

        mWorldView.save(writer);
        ESM::LuaScripts globalScripts;
        mGlobalScripts.save(globalScripts);
        globalScripts.save(writer);
        saveEvents(writer, mGlobalEvents, mLocalEvents);

        writer.endRecord(ESM::REC_LUAM);
    }

    void LuaManager::readRecord(ESM::ESMReader& reader, uint32_t type)
    {
        if (type != ESM::REC_LUAM)
            throw std::runtime_error("ESM::REC_LUAM is expected");

        mWorldView.load(reader);
        ESM::LuaScripts globalScripts;
        globalScripts.load(reader);
        loadEvents(mLua.sol(), reader, mGlobalEvents, mLocalEvents, mContentFileMapping, mGlobalLoader.get());

        mGlobalScripts.setSerializer(mGlobalLoader.get());
        mGlobalScripts.load(globalScripts, false);
        mGlobalScripts.setSerializer(mGlobalSerializer.get());
    }

    void LuaManager::saveLocalScripts(const MWWorld::Ptr& ptr, ESM::LuaScripts& data)
    {
        if (ptr.getRefData().getLuaScripts())
            ptr.getRefData().getLuaScripts()->save(data);
        else
            data.mScripts.clear();
    }

    void LuaManager::loadLocalScripts(const MWWorld::Ptr& ptr, const ESM::LuaScripts& data)
    {
        if (data.mScripts.empty())
        {
            if (ptr.getRefData().getLuaScripts())
                ptr.getRefData().setLuaScripts(nullptr);
            return;
        }

        mWorldView.getObjectRegistry()->registerPtr(ptr);
        LocalScripts* scripts = createLocalScripts(ptr);

        scripts->setSerializer(mLocalLoader.get());
        scripts->load(data, true);
        scripts->setSerializer(mLocalSerializer.get());

        // LiveCellRef is usually copied after loading, so this Ptr will become invalid and should be deregistered.
        mWorldView.getObjectRegistry()->deregisterPtr(ptr);
    }

    void LuaManager::reloadAllScripts()
    {
        Log(Debug::Info) << "Reload Lua";
        mLua.dropScriptCache();

        {  // Reload global scripts
            ESM::LuaScripts data;
            mGlobalScripts.save(data);
            mGlobalScripts.removeAllScripts();
            for (const std::string& path : mGlobalScriptList)
                if (mGlobalScripts.addNewScript(path))
                    Log(Debug::Info) << "Global script restarted: " << path;
            mGlobalScripts.load(data, false);
        }

        for (const auto& [id, ptr] : mWorldView.getObjectRegistry()->mObjectMapping)
        {  // Reload local scripts
            LocalScripts* scripts = ptr.getRefData().getLuaScripts();
            if (scripts == nullptr)
                continue;
            ESM::LuaScripts data;
            scripts->save(data);
            scripts->load(data, true);
        }
    }

}
