#include <BulletCollision/BroadphaseCollision/btDbvtBroadphase.h>
#include <BulletCollision/CollisionShapes/btCollisionShape.h>

#include <osg/Stats>

#include "components/debug/debuglog.hpp"
#include <components/misc/barrier.hpp>
#include "components/misc/convert.hpp"
#include "components/settings/settings.hpp"
#include "../mwmechanics/actorutil.hpp"
#include "../mwmechanics/movement.hpp"
#include "../mwrender/bulletdebugdraw.hpp"
#include "../mwworld/class.hpp"

#include "actor.hpp"
#include "contacttestwrapper.h"
#include "movementsolver.hpp"
#include "mtphysics.hpp"
#include "object.hpp"
#include "physicssystem.hpp"
#include "projectile.hpp"

namespace
{
    /// @brief A scoped lock that is either shared or exclusive depending on configuration
    template<class Mutex>
    class MaybeSharedLock
    {
        public:
            /// @param mutex a shared mutex
            /// @param canBeSharedLock decide wether the lock will be shared or exclusive
            MaybeSharedLock(Mutex& mutex, bool canBeSharedLock) : mMutex(mutex), mCanBeSharedLock(canBeSharedLock)
            {
                if (mCanBeSharedLock)
                    mMutex.lock_shared();
                else
                    mMutex.lock();
            }

            ~MaybeSharedLock()
            {
                if (mCanBeSharedLock)
                    mMutex.unlock_shared();
                else
                    mMutex.unlock();
            }
        private:
            Mutex& mMutex;
            bool mCanBeSharedLock;
    };

    bool isUnderWater(const MWPhysics::ActorFrameData& actorData)
    {
        return actorData.mPosition.z() < actorData.mSwimLevel;
    }

    void handleFall(MWPhysics::ActorFrameData& actorData, bool simulationPerformed)
    {
        const float heightDiff = actorData.mPosition.z() - actorData.mOldHeight;

        const bool isStillOnGround = (simulationPerformed && actorData.mWasOnGround && actorData.mIsOnGround);

        if (isStillOnGround || actorData.mFlying || isUnderWater(actorData) || actorData.mSlowFall < 1)
            actorData.mNeedLand = true;
        else if (heightDiff < 0)
            actorData.mFallHeight += heightDiff;
    }

    void updateMechanics(MWPhysics::Actor& actor, MWPhysics::ActorFrameData& actorData)
    {
        auto ptr = actor.getPtr();

        MWMechanics::CreatureStats& stats = ptr.getClass().getCreatureStats(ptr);
        if (actorData.mNeedLand)
            stats.land(ptr == MWMechanics::getPlayer() && (actorData.mFlying || isUnderWater(actorData)));
        else if (actorData.mFallHeight < 0)
            stats.addToFallHeight(-actorData.mFallHeight);
    }

    osg::Vec3f interpolateMovements(MWPhysics::Actor& actor, MWPhysics::ActorFrameData& actorData, float timeAccum, float physicsDt)
    {
        const float interpolationFactor = std::clamp(timeAccum / physicsDt, 0.0f, 1.0f);
        return actorData.mPosition * interpolationFactor + actor.getPreviousPosition() * (1.f - interpolationFactor);
    }

    namespace Config
    {
        /// @return either the number of thread as configured by the user, or 1 if Bullet doesn't support multithreading
        int computeNumThreads(bool& threadSafeBullet)
        {
            int wantedThread = Settings::Manager::getInt("async num threads", "Physics");

            auto broad = std::make_unique<btDbvtBroadphase>();
            auto maxSupportedThreads = broad->m_rayTestStacks.size();
            threadSafeBullet = (maxSupportedThreads > 1);
            if (!threadSafeBullet && wantedThread > 1)
            {
                Log(Debug::Warning) << "Bullet was not compiled with multithreading support, 1 async thread will be used";
                return 1;
            }
            return std::max(0, wantedThread);
        }
    }
}

namespace MWPhysics
{
    PhysicsTaskScheduler::PhysicsTaskScheduler(float physicsDt, btCollisionWorld *collisionWorld, MWRender::DebugDrawer* debugDrawer)
          : mDefaultPhysicsDt(physicsDt)
          , mPhysicsDt(physicsDt)
          , mTimeAccum(0.f)
          , mCollisionWorld(collisionWorld)
          , mDebugDrawer(debugDrawer)
          , mNumJobs(0)
          , mRemainingSteps(0)
          , mLOSCacheExpiry(Settings::Manager::getInt("lineofsight keep inactive cache", "Physics"))
          , mNewFrame(false)
          , mAdvanceSimulation(false)
          , mQuit(false)
          , mNextJob(0)
          , mNextLOS(0)
          , mFrameNumber(0)
          , mTimer(osg::Timer::instance())
          , mPrevStepCount(1)
          , mBudget(physicsDt)
          , mAsyncBudget(0.0f)
          , mBudgetCursor(0)
          , mAsyncStartTime(0)
          , mTimeBegin(0)
          , mTimeEnd(0)
          , mFrameStart(0)
    {
        mNumThreads = Config::computeNumThreads(mThreadSafeBullet);

        if (mNumThreads >= 1)
        {
            for (int i = 0; i < mNumThreads; ++i)
                mThreads.emplace_back([&] { worker(); } );
        }
        else
        {
            mLOSCacheExpiry = 0;
        }

        mPreStepBarrier = std::make_unique<Misc::Barrier>(mNumThreads);

        mPostStepBarrier = std::make_unique<Misc::Barrier>(mNumThreads);

        mPostSimBarrier = std::make_unique<Misc::Barrier>(mNumThreads);
    }

    PhysicsTaskScheduler::~PhysicsTaskScheduler()
    {
        std::unique_lock lock(mSimulationMutex);
        mQuit = true;
        mNumJobs = 0;
        mRemainingSteps = 0;
        lock.unlock();
        mHasJob.notify_all();
        for (auto& thread : mThreads)
            thread.join();
    }

    std::tuple<int, float> PhysicsTaskScheduler::calculateStepConfig(float timeAccum) const
    {
        int maxAllowedSteps = 2;
        int numSteps = timeAccum / mDefaultPhysicsDt;

        // adjust maximum step count based on whether we're likely physics bottlenecked or not
        // if maxAllowedSteps ends up higher than numSteps, we will not invoke delta time
        // if it ends up lower than numSteps, but greater than 1, we will run a number of true delta time physics steps that we expect to be within budget
        // if it ends up lower than numSteps and also 1, we will run a single delta time physics step
        // if we did not do this, and had a fixed step count limit,
        // we would have an unnecessarily low render framerate if we were only physics bottlenecked,
        // and we would be unnecessarily invoking true delta time if we were only render bottlenecked

        // get physics timing stats
        float budgetMeasurement = std::max(mBudget.get(), mAsyncBudget.get());
        // time spent per step in terms of the intended physics framerate
        budgetMeasurement /= mDefaultPhysicsDt;
        // ensure sane minimum value
        budgetMeasurement = std::max(0.00001f, budgetMeasurement);
        // we're spending almost or more than realtime per physics frame; limit to a single step
        if (budgetMeasurement > 0.95)
            maxAllowedSteps = 1;
        // physics is fairly cheap; limit based on expense
        if (budgetMeasurement < 0.5)
            maxAllowedSteps = std::ceil(1.0/budgetMeasurement);
        // limit to a reasonable amount
        maxAllowedSteps = std::min(10, maxAllowedSteps);

        // fall back to delta time for this frame if fixed timestep physics would fall behind
        float actualDelta = mDefaultPhysicsDt;
        if (numSteps > maxAllowedSteps)
        {
            numSteps = maxAllowedSteps;
            // ensure that we do not simulate a frame ahead when doing delta time; this reduces stutter and latency
            // this causes interpolation to 100% use the most recent physics result when true delta time is happening
            // and we deliberately simulate up to exactly the timestamp that we want to render
            actualDelta = timeAccum/float(numSteps+1);
            // actually: if this results in a per-step delta less than the target physics steptime, clamp it
            // this might reintroduce some stutter, but only comes into play in obscure cases
            // (because numSteps is originally based on mDefaultPhysicsDt, this won't cause us to overrun)
            actualDelta = std::max(actualDelta, mDefaultPhysicsDt);
        }

        return std::make_tuple(numSteps, actualDelta);
    }

    void PhysicsTaskScheduler::applyQueuedMovements(float & timeAccum, std::vector<std::shared_ptr<Actor>>&& actors, std::vector<ActorFrameData>&& actorsData, osg::Timer_t frameStart, unsigned int frameNumber, osg::Stats& stats)
    {
        // This function run in the main thread.
        // While the mSimulationMutex is held, background physics threads can't run.

        std::unique_lock lock(mSimulationMutex);
        assert(actors.size() == actorsData.size());

        double timeStart = mTimer->tick();

        // start by finishing previous background computation
        if (mNumThreads != 0)
        {
            for (size_t i = 0; i < mActors.size(); ++i)
            {
                updateMechanics(*mActors[i], mActorsFrameData[i]);
                updateActor(*mActors[i], mActorsFrameData[i], mAdvanceSimulation, mTimeAccum, mPhysicsDt);
            }
            if(mAdvanceSimulation)
                mAsyncBudget.update(mTimer->delta_s(mAsyncStartTime, mTimeEnd), mPrevStepCount, mBudgetCursor);
            updateStats(frameStart, frameNumber, stats);
        }

        auto [numSteps, newDelta] = calculateStepConfig(timeAccum);
        timeAccum -= numSteps*newDelta;

        // init
        for (size_t i = 0; i < actors.size(); ++i)
        {
            actorsData[i].updatePosition(*actors[i], mCollisionWorld);
        }
        mPrevStepCount = numSteps;
        mRemainingSteps = numSteps;
        mTimeAccum = timeAccum;
        mPhysicsDt = newDelta;
        mActors = std::move(actors);
        mActorsFrameData = std::move(actorsData);
        mAdvanceSimulation = (mRemainingSteps != 0);
        mNewFrame = true;
        mNumJobs = mActorsFrameData.size();
        mNextLOS.store(0, std::memory_order_relaxed);
        mNextJob.store(0, std::memory_order_release);

        if (mAdvanceSimulation)
            mWorldFrameData = std::make_unique<WorldFrameData>();

        if (mAdvanceSimulation)
            mBudgetCursor += 1;

        if (mNumThreads == 0)
        {
            syncComputation();
            if(mAdvanceSimulation)
                mBudget.update(mTimer->delta_s(timeStart, mTimer->tick()), numSteps, mBudgetCursor);
            return;
        }

        mAsyncStartTime = mTimer->tick();
        lock.unlock();
        mHasJob.notify_all();
        if (mAdvanceSimulation)
            mBudget.update(mTimer->delta_s(timeStart, mTimer->tick()), 1, mBudgetCursor);
    }

    void PhysicsTaskScheduler::resetSimulation(const ActorMap& actors)
    {
        std::unique_lock lock(mSimulationMutex);
        mBudget.reset(mDefaultPhysicsDt);
        mAsyncBudget.reset(0.0f);
        mActors.clear();
        mActorsFrameData.clear();
        for (const auto& [_, actor] : actors)
        {
            actor->updatePosition();
            actor->updateCollisionObjectPosition();
        }
    }

    void PhysicsTaskScheduler::rayTest(const btVector3& rayFromWorld, const btVector3& rayToWorld, btCollisionWorld::RayResultCallback& resultCallback) const
    {
        MaybeSharedLock lock(mCollisionWorldMutex, mThreadSafeBullet);
        mCollisionWorld->rayTest(rayFromWorld, rayToWorld, resultCallback);
    }

    void PhysicsTaskScheduler::convexSweepTest(const btConvexShape* castShape, const btTransform& from, const btTransform& to, btCollisionWorld::ConvexResultCallback& resultCallback) const
    {
        MaybeSharedLock lock(mCollisionWorldMutex, mThreadSafeBullet);
        mCollisionWorld->convexSweepTest(castShape, from, to, resultCallback);
    }

    void PhysicsTaskScheduler::contactTest(btCollisionObject* colObj, btCollisionWorld::ContactResultCallback& resultCallback)
    {
        std::shared_lock lock(mCollisionWorldMutex);
        ContactTestWrapper::contactTest(mCollisionWorld, colObj, resultCallback);
    }

    std::optional<btVector3> PhysicsTaskScheduler::getHitPoint(const btTransform& from, btCollisionObject* target)
    {
        MaybeSharedLock lock(mCollisionWorldMutex, mThreadSafeBullet);
        // target the collision object's world origin, this should be the center of the collision object
        btTransform rayTo;
        rayTo.setIdentity();
        rayTo.setOrigin(target->getWorldTransform().getOrigin());

        btCollisionWorld::ClosestRayResultCallback cb(from.getOrigin(), rayTo.getOrigin());

        mCollisionWorld->rayTestSingle(from, rayTo, target, target->getCollisionShape(), target->getWorldTransform(), cb);
        if (!cb.hasHit())
            // didn't hit the target. this could happen if point is already inside the collision box
            return std::nullopt;
        return {cb.m_hitPointWorld};
    }

    void PhysicsTaskScheduler::aabbTest(const btVector3& aabbMin, const btVector3& aabbMax, btBroadphaseAabbCallback& callback)
    {
        std::shared_lock lock(mCollisionWorldMutex);
        mCollisionWorld->getBroadphase()->aabbTest(aabbMin, aabbMax, callback);
    }

    void PhysicsTaskScheduler::getAabb(const btCollisionObject* obj, btVector3& min, btVector3& max)
    {
        std::shared_lock lock(mCollisionWorldMutex);
        obj->getCollisionShape()->getAabb(obj->getWorldTransform(), min, max);
    }

    void PhysicsTaskScheduler::setCollisionFilterMask(btCollisionObject* collisionObject, int collisionFilterMask)
    {
        std::unique_lock lock(mCollisionWorldMutex);
        collisionObject->getBroadphaseHandle()->m_collisionFilterMask = collisionFilterMask;
    }

    void PhysicsTaskScheduler::addCollisionObject(btCollisionObject* collisionObject, int collisionFilterGroup, int collisionFilterMask)
    {
        mCollisionObjects.insert(collisionObject);
        std::unique_lock lock(mCollisionWorldMutex);
        mCollisionWorld->addCollisionObject(collisionObject, collisionFilterGroup, collisionFilterMask);
    }

    void PhysicsTaskScheduler::removeCollisionObject(btCollisionObject* collisionObject)
    {
        mCollisionObjects.erase(collisionObject);
        std::unique_lock lock(mCollisionWorldMutex);
        mCollisionWorld->removeCollisionObject(collisionObject);
    }

    void PhysicsTaskScheduler::updateSingleAabb(std::shared_ptr<PtrHolder> ptr, bool immediate)
    {
        if (immediate || mNumThreads == 0)
        {
            updatePtrAabb(ptr);
        }
        else
        {
            std::unique_lock lock(mUpdateAabbMutex);
            mUpdateAabb.insert(std::move(ptr));
        }
    }

    bool PhysicsTaskScheduler::getLineOfSight(const std::shared_ptr<Actor>& actor1, const std::shared_ptr<Actor>& actor2)
    {
        std::unique_lock lock(mLOSCacheMutex);

        auto req = LOSRequest(actor1, actor2);
        auto result = std::find(mLOSCache.begin(), mLOSCache.end(), req);
        if (result == mLOSCache.end())
        {
            req.mResult = hasLineOfSight(actor1.get(), actor2.get());
            mLOSCache.push_back(req);
            return req.mResult;
        }
        result->mAge = 0;
        return result->mResult;
    }

    void PhysicsTaskScheduler::refreshLOSCache()
    {
        std::shared_lock lock(mLOSCacheMutex);
        int job = 0;
        int numLOS = mLOSCache.size();
        while ((job = mNextLOS.fetch_add(1, std::memory_order_relaxed)) < numLOS)
        {
            auto& req = mLOSCache[job];
            auto actorPtr1 = req.mActors[0].lock();
            auto actorPtr2 = req.mActors[1].lock();

            if (req.mAge++ > mLOSCacheExpiry || !actorPtr1 || !actorPtr2)
                req.mStale = true;
            else
                req.mResult = hasLineOfSight(actorPtr1.get(), actorPtr2.get());
        }

    }

    void PhysicsTaskScheduler::updateAabbs()
    {
        std::scoped_lock lock(mUpdateAabbMutex);
        std::for_each(mUpdateAabb.begin(), mUpdateAabb.end(),
            [this](const std::shared_ptr<PtrHolder>& ptr) { updatePtrAabb(ptr); });
        mUpdateAabb.clear();
    }

    void PhysicsTaskScheduler::updatePtrAabb(const std::shared_ptr<PtrHolder>& ptr)
    {
        std::scoped_lock lock(mCollisionWorldMutex);
        if (const auto actor = std::dynamic_pointer_cast<Actor>(ptr))
        {
            actor->updateCollisionObjectPosition();
            mCollisionWorld->updateSingleAabb(actor->getCollisionObject());
        }
        else if (const auto object = std::dynamic_pointer_cast<Object>(ptr))
        {
            object->commitPositionChange();
            mCollisionWorld->updateSingleAabb(object->getCollisionObject());
        }
        else if (const auto projectile = std::dynamic_pointer_cast<Projectile>(ptr))
        {
            projectile->commitPositionChange();
            mCollisionWorld->updateSingleAabb(projectile->getCollisionObject());
        }
    }

    void PhysicsTaskScheduler::worker()
    {
        std::shared_lock lock(mSimulationMutex);
        while (!mQuit)
        {
            if (!mNewFrame)
                mHasJob.wait(lock, [&]() { return mQuit || mNewFrame; });

            mPreStepBarrier->wait([this] { afterPreStep(); });

            int job = 0;
            while (mRemainingSteps && (job = mNextJob.fetch_add(1, std::memory_order_relaxed)) < mNumJobs)
            {
                MaybeSharedLock lockColWorld(mCollisionWorldMutex, mThreadSafeBullet);
                MovementSolver::move(mActorsFrameData[job], mPhysicsDt, mCollisionWorld, *mWorldFrameData);
            }

            mPostStepBarrier->wait([this] { afterPostStep(); });

            if (!mRemainingSteps)
            {
                while ((job = mNextJob.fetch_add(1, std::memory_order_relaxed)) < mNumJobs)
                {
                    handleFall(mActorsFrameData[job], mAdvanceSimulation);
                }

                refreshLOSCache();
                mPostSimBarrier->wait([this] { afterPostSim(); });
            }
        }
    }

    void PhysicsTaskScheduler::updateActorsPositions()
    {
        for (size_t i = 0; i < mActors.size(); ++i)
        {
            if (mActors[i]->setPosition(mActorsFrameData[i].mPosition))
            {
                std::scoped_lock lock(mCollisionWorldMutex);
                mActorsFrameData[i].mPosition = mActors[i]->getPosition(); // account for potential position change made by script
                mActors[i]->updateCollisionObjectPosition();
                mCollisionWorld->updateSingleAabb(mActors[i]->getCollisionObject());
            }
        }
    }

    void PhysicsTaskScheduler::updateActor(Actor& actor, ActorFrameData& actorData, bool simulationPerformed, float timeAccum, float dt) const
    {
        actor.setSimulationPosition(interpolateMovements(actor, actorData, timeAccum, dt));
        actor.setLastStuckPosition(actorData.mLastStuckPosition);
        actor.setStuckFrames(actorData.mStuckFrames);
        if (simulationPerformed)
        {
            MWWorld::Ptr standingOn;
            auto* ptrHolder = static_cast<MWPhysics::PtrHolder*>(getUserPointer(actorData.mStandingOn));
            if (ptrHolder)
                standingOn = ptrHolder->getPtr();
            actor.setStandingOnPtr(standingOn);
            // the "on ground" state of an actor might have been updated by a traceDown, don't overwrite the change
            if (actor.getOnGround() == actorData.mWasOnGround)
                actor.setOnGround(actorData.mIsOnGround);
            actor.setOnSlope(actorData.mIsOnSlope);
            actor.setWalkingOnWater(actorData.mWalkingOnWater);
            actor.setInertialForce(actorData.mInertia);
        }
    }

    bool PhysicsTaskScheduler::hasLineOfSight(const Actor* actor1, const Actor* actor2)
    {
        btVector3 pos1  = Misc::Convert::toBullet(actor1->getCollisionObjectPosition() + osg::Vec3f(0,0,actor1->getHalfExtents().z() * 0.9)); // eye level
        btVector3 pos2  = Misc::Convert::toBullet(actor2->getCollisionObjectPosition() + osg::Vec3f(0,0,actor2->getHalfExtents().z() * 0.9));

        btCollisionWorld::ClosestRayResultCallback resultCallback(pos1, pos2);
        resultCallback.m_collisionFilterGroup = 0xFF;
        resultCallback.m_collisionFilterMask = CollisionType_World|CollisionType_HeightMap|CollisionType_Door;

        MaybeSharedLock lockColWorld(mCollisionWorldMutex, mThreadSafeBullet);
        mCollisionWorld->rayTest(pos1, pos2, resultCallback);

        return !resultCallback.hasHit();
    }

    void PhysicsTaskScheduler::syncComputation()
    {
        while (mRemainingSteps--)
        {
            for (auto& actorData : mActorsFrameData)
            {
                MovementSolver::unstuck(actorData, mCollisionWorld);
                MovementSolver::move(actorData, mPhysicsDt, mCollisionWorld, *mWorldFrameData);
            }

            updateActorsPositions();
        }

        for (size_t i = 0; i < mActors.size(); ++i)
        {
            handleFall(mActorsFrameData[i], mAdvanceSimulation);
            updateMechanics(*mActors[i], mActorsFrameData[i]);
            updateActor(*mActors[i], mActorsFrameData[i], mAdvanceSimulation, mTimeAccum, mPhysicsDt);
        }
        refreshLOSCache();
    }

    void PhysicsTaskScheduler::updateStats(osg::Timer_t frameStart, unsigned int frameNumber, osg::Stats& stats)
    {
        if (!stats.collectStats("engine"))
            return;
        if (mFrameNumber == frameNumber - 1)
        {
            stats.setAttribute(mFrameNumber, "physicsworker_time_begin", mTimer->delta_s(mFrameStart, mTimeBegin));
            stats.setAttribute(mFrameNumber, "physicsworker_time_taken", mTimer->delta_s(mTimeBegin, mTimeEnd));
            stats.setAttribute(mFrameNumber, "physicsworker_time_end", mTimer->delta_s(mFrameStart, mTimeEnd));
        }
        mFrameStart = frameStart;
        mTimeBegin = mTimer->tick();
        mFrameNumber = frameNumber;
    }

    void PhysicsTaskScheduler::debugDraw()
    {
        std::shared_lock lock(mCollisionWorldMutex);
        mDebugDrawer->step();
    }

    void* PhysicsTaskScheduler::getUserPointer(const btCollisionObject* object) const
    {
        auto it = mCollisionObjects.find(object);
        if (it == mCollisionObjects.end())
            return nullptr;
        return (*it)->getUserPointer();
    }

    void PhysicsTaskScheduler::releaseSharedStates()
    {
        std::scoped_lock lock(mSimulationMutex, mUpdateAabbMutex);
        mActors.clear();
        mUpdateAabb.clear();
    }

    void PhysicsTaskScheduler::afterPreStep()
    {
        updateAabbs();
        if (!mRemainingSteps)
            return;
        for (size_t i = 0; i < mActors.size(); ++i)
        {
            std::unique_lock lock(mCollisionWorldMutex);
            MovementSolver::unstuck(mActorsFrameData[i], mCollisionWorld);
        }
    }

    void PhysicsTaskScheduler::afterPostStep()
    {
        if (mRemainingSteps)
        {
            --mRemainingSteps;
            updateActorsPositions();
        }
        mNextJob.store(0, std::memory_order_release);
    }

    void PhysicsTaskScheduler::afterPostSim()
    {
        mNewFrame = false;
        {
            std::unique_lock lock(mLOSCacheMutex);
            mLOSCache.erase(
                    std::remove_if(mLOSCache.begin(), mLOSCache.end(),
                        [](const LOSRequest& req) { return req.mStale; }),
                    mLOSCache.end());
        }
        mTimeEnd = mTimer->tick();
    }
}
