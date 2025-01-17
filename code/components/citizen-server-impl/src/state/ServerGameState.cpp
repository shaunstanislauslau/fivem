#include <StdInc.h>
#include <GameServer.h>

#include <state/ServerGameState.h>

#include <optional>

#include <NetBuffer.h>

#include <lz4.h>

#include <tbb/concurrent_queue.h>
#include <thread_pool.hpp>

#include <state/Pool.h>

#include <IteratorView.h>

#include <ResourceEventComponent.h>
#include <ResourceManager.h>

#include <DebugAlias.h>

#define GLM_ENABLE_EXPERIMENTAL

// TODO: clang style defines/checking
#if defined(_M_IX86) || defined(_M_AMD64)
#define GLM_FORCE_DEFAULT_ALIGNED_GENTYPES
#define GLM_FORCE_SSE2
#define GLM_FORCE_SSE3
#endif

#include <glm/vec3.hpp>
#include <glm/vec4.hpp>
#include <glm/mat4x4.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtx/euler_angles.hpp>
#include <glm/gtx/quaternion.hpp>

static constexpr const int MaxObjectId = 1 << 13;

CPool<fx::ScriptGuid>* g_scriptHandlePool;

std::shared_ptr<ConVar<bool>> g_oneSyncVar;
std::shared_ptr<ConVar<bool>> g_oneSyncCulling;
std::shared_ptr<ConVar<bool>> g_oneSyncRadiusFrequency;
std::shared_ptr<ConVar<std::string>> g_oneSyncLogVar;

static tbb::concurrent_queue<std::string> g_logQueue;

static std::condition_variable g_consoleCondVar;
static std::mutex g_consoleMutex;

static std::once_flag g_logOnceFlag;

static void Log(const char* format, const fmt::ArgList& argumentList)
{
	if (!g_oneSyncLogVar->GetValue().empty())
	{
		std::call_once(g_logOnceFlag, []()
		{
			std::thread([]()
			{
				while (true)
				{
					{
						std::unique_lock<std::mutex> lock(g_consoleMutex);
						g_consoleCondVar.wait(lock);
					}

					static std::string lastLogFile;
					static FILE* file;

					if (lastLogFile != g_oneSyncLogVar->GetValue())
					{
						if (file)
						{
							fclose(file);
							file = nullptr;
						}

						if (!g_oneSyncLogVar->GetValue().empty())
						{
							file = _pfopen(MakeRelativeCitPath(g_oneSyncLogVar->GetValue()).c_str(), _P("w"));
						}

						lastLogFile = g_oneSyncLogVar->GetValue();
					}

					std::string str;

					while (g_logQueue.try_pop(str))
					{
						if (file)
						{
							fprintf(file, "%s", str.c_str());
						}
					}
				}
			}).detach();
		});

		g_logQueue.push(fmt::sprintf("[% 10d] ", msec().count()));
		g_logQueue.push(fmt::sprintf(format, argumentList));

		g_consoleCondVar.notify_all();
	}
}

FMT_VARIADIC(void, Log, const char*);

#define GS_LOG(x, ...) \
	do \
	{ \
		if (!g_oneSyncLogVar->GetValue().empty()) \
		{ \
			Log(x, __VA_ARGS__); \
		} \
	} while (false)

namespace fx
{
static const glm::mat4x4 g_projectionMatrix = glm::perspective(90.0f, 4.f / 3.f, 0.1f, 1000.f);

struct ViewClips
{
	glm::vec4 nearClip;
	glm::vec4 farClip;
	glm::vec4 topClip;
	glm::vec4 bottomClip;
	glm::vec4 leftClip;
	glm::vec4 rightClip;

	ViewClips(const glm::mat4x4& matrix)
	{
		auto tpmatrix = glm::transpose(matrix);

		leftClip	= tpmatrix * glm::vec4{ 1.0f, 0.0f, 0.0f, 1.0f };
		rightClip	= tpmatrix * glm::vec4{ -1.0f, 0.0f, 0.0f, 1.0f };
		bottomClip	= tpmatrix * glm::vec4{ 0.0f, 1.0f, 0.0f, 1.0f };
		topClip		= tpmatrix * glm::vec4{ 0.0f, -1.0f, 0.0f, 1.0f };
		nearClip	= tpmatrix * glm::vec4{ 0.0f, 0.0f, 1.0f, 1.0f };
		farClip		= tpmatrix * glm::vec4{ 0.0f, 0.0f, -1.0f, 1.0f };
	}
};

static const ViewClips g_projectionClips{ g_projectionMatrix };

static bool IsInFrustum(const glm::vec3& pos, float radius, const glm::mat4x4& viewMatrix)
{
	auto viewCoords = viewMatrix * glm::vec4{ pos, 1.0f };

	auto testPlane = [&viewCoords, &radius](const glm::vec4& plane)
	{
		glm::vec3 mcoords = viewCoords * plane;

		return (mcoords.x + mcoords.y + mcoords.z + plane.w + radius) >= 0.0f;
	};

	return (testPlane(g_projectionClips.nearClip)
		&& testPlane(g_projectionClips.bottomClip)
		&& testPlane(g_projectionClips.topClip)
		&& testPlane(g_projectionClips.leftClip)
		&& testPlane(g_projectionClips.rightClip)
		&& testPlane(g_projectionClips.farClip));
}

sync::SyncEntityState::SyncEntityState()
	: deleting(false)
{

}

struct GameStateClientData : public sync::ClientSyncDataBase
{
	rl::MessageBuffer ackBuffer{ 16384 };
	std::set<int> objectIds;

	std::mutex selfMutex;

	std::weak_ptr<sync::SyncEntityState> playerEntity;
	std::optional<int> playerId;

	bool syncing;

	glm::mat4x4 viewMatrix;

	std::unordered_multimap<uint64_t, uint16_t> idsForGameState;

	eastl::bitset<MaxObjectId> pendingRemovals;

	std::weak_ptr<fx::Client> client;

	GameStateClientData()
		: syncing(false)
	{

	}

	void FlushAcks();

	void MaybeFlushAcks();
};

inline std::shared_ptr<GameStateClientData> GetClientDataUnlocked(ServerGameState* state, const std::shared_ptr<fx::Client>& client)
{
	// NOTE: static_pointer_cast typically will lead to an unneeded refcount increment+decrement
	// Doing this makes it so that there's only *one* increment for the fast case.
#ifndef _MSC_VER
	auto data = std::static_pointer_cast<GameStateClientData>(client->GetSyncData());
#else
	auto data = std::shared_ptr<GameStateClientData>{ reinterpret_cast<std::shared_ptr<GameStateClientData>&&>(client->GetSyncData()) };
#endif

	if (!data)
	{
		data = std::make_shared<GameStateClientData>();
		data->client = client;

		client->SetSyncData(data);

		std::weak_ptr<fx::Client> weakClient(client);

		client->OnDrop.Connect([weakClient, state]()
		{
			state->HandleClientDrop(weakClient.lock());
		});
	}

	return data;
}

inline std::tuple<std::shared_ptr<GameStateClientData>, std::unique_lock<std::mutex>> GetClientData(ServerGameState* state, const std::shared_ptr<fx::Client>& client)
{
	auto val = GetClientDataUnlocked(state, client);

	std::unique_lock<std::mutex> lock(val->selfMutex);
	return { val, std::move(lock) };
}

inline uint32_t MakeEntityHandle(uint8_t playerId, uint16_t objectId)
{
	return ((playerId + 1) << 16) | objectId;
}

uint32_t MakeScriptHandle(const std::shared_ptr<sync::SyncEntityState>& ptr)
{
	if (!ptr->guid)
	{
		// find an existing handle (transformed TempEntity?)
		for (int i = 0; i < g_scriptHandlePool->m_Size; i++)
		{
			auto hdl = g_scriptHandlePool->GetAt(i);

			if (hdl && hdl->type == ScriptGuid::Type::Entity && hdl->entity.handle == ptr->handle)
			{
				ptr->guid = hdl;
			}
		}

		if (!ptr->guid)
		{
			auto guid = g_scriptHandlePool->New();
			guid->type = ScriptGuid::Type::Entity;
			guid->entity.handle = ptr->handle;

			ptr->guid = guid;
		}
	}

	return g_scriptHandlePool->GetIndex(ptr->guid) + 0x20000;
}

inline glm::vec3 GetPlayerFocusPos(const std::shared_ptr<sync::SyncEntityState>& entity)
{
	if (!entity->syncTree)
	{
		return { 0, 0, 0 };
	}

	float playerPos[3];
	entity->syncTree->GetPosition(playerPos);

	auto camData = entity->syncTree->GetPlayerCamera();

	if (!camData)
	{
		return { playerPos[0], playerPos[1], playerPos[2] };
	}

	switch (camData->camMode)
	{
	case 0:
	default:
		return { playerPos[0], playerPos[1], playerPos[2] };
	case 1:
		return { camData->freeCamPosX, camData->freeCamPosY, camData->freeCamPosZ };
	case 2:
		return { playerPos[0] + camData->camOffX, playerPos[1] + camData->camOffY, playerPos[2] + camData->camOffZ };
	}
}

ServerGameState::ServerGameState()
	: m_frameIndex(0), m_entitiesById(1 << 13)
{
	m_tg = std::make_unique<ThreadPool>();
}

std::shared_ptr<sync::SyncEntityState> ServerGameState::GetEntity(uint8_t playerId, uint16_t objectId)
{
	if (objectId >= m_entitiesById.size() || objectId < 0)
	{
		return {};
	}

	uint16_t objIdAlias = objectId;
	debug::Alias(&objIdAlias);

	std::unique_lock<std::mutex> lock(m_entitiesByIdMutex);
	auto ptr = m_entitiesById[objectId];

	return ptr.lock();
}

std::shared_ptr<sync::SyncEntityState> ServerGameState::GetEntity(uint32_t guid)
{
	// subtract the minimum index GUID
	guid -= 0x20000;

	// get the pool entry
	auto guidData = g_scriptHandlePool->AtHandle(guid);

	if (guidData)
	{
		if (guidData->type == ScriptGuid::Type::Entity)
		{
			std::unique_lock<std::mutex> lock(m_entitiesByIdMutex);
			auto ptr = m_entitiesById[guidData->entity.handle & 0xFFFF];

			return ptr.lock();
		}
	}

	return {};
}

namespace sync
{
	SyncEntityState::~SyncEntityState()
	{
		if (guid)
		{
			g_scriptHandlePool->Delete(guid);

			guid = nullptr;
		}
	}
}

struct SyncCommandState
{
	rl::MessageBuffer cloneBuffer;
	std::function<void()> flushBuffer;
	std::function<void()> maybeFlushBuffer;
	uint64_t frameIndex;
	std::shared_ptr<fx::Client> client;

	SyncCommandState(size_t size)
		: cloneBuffer(size)
	{

	}
};

using SyncCommand = tp::FixedFunction<void(SyncCommandState&), 128>;

struct SyncCommandList
{
	uint64_t frameIndex;
	std::shared_ptr<fx::Client> client;

	std::list<SyncCommand> commands;

	void Execute();
};

static void FlushBuffer(rl::MessageBuffer& buffer, uint32_t msgType, uint64_t frameIndex, const std::shared_ptr<fx::Client>& client)
{
	if (buffer.GetDataLength() > 0)
	{
		// end
		buffer.Write(3, 7);

		// compress and send
		std::vector<char> outData(LZ4_compressBound(buffer.GetDataLength()) + 4 + 8);
		int len = LZ4_compress_default(reinterpret_cast<const char*>(buffer.GetBuffer().data()), outData.data() + 4 + 8, buffer.GetDataLength(), outData.size() - 4 - 8);

		*(uint32_t*)(outData.data()) = msgType;
		*(uint64_t*)(outData.data() + 4) = frameIndex;

		net::Buffer netBuffer(reinterpret_cast<uint8_t*>(outData.data()), len + 4 + 8);
		netBuffer.Seek(len + 4 + 8); // since the buffer constructor doesn't actually set the offset

		GS_LOG("flushBuffer: sending %d bytes to %d\n", len + 4 + 8, client->GetNetId());

		client->SendPacket(1, netBuffer);

		size_t oldCurrentBit = buffer.GetCurrentBit();

		debug::Alias(&oldCurrentBit);
		debug::Alias(&len);

		buffer.SetCurrentBit(0);
	}
}

static void MaybeFlushBuffer(rl::MessageBuffer& buffer, uint32_t msgType, uint64_t frameIndex, const std::shared_ptr<fx::Client>& client)
{
	if (LZ4_compressBound(buffer.GetDataLength()) > 1100)
	{
		FlushBuffer(buffer, msgType, frameIndex, client);
	}
}

void GameStateClientData::FlushAcks()
{
	auto clientRef = client.lock();

	if (clientRef)
	{
		FlushBuffer(ackBuffer, HashRageString("msgPackedAcks"), 0, clientRef);
	}
}

void GameStateClientData::MaybeFlushAcks()
{
	auto clientRef = client.lock();

	if (clientRef)
	{
		MaybeFlushBuffer(ackBuffer, HashRageString("msgPackedAcks"), 0, clientRef);
	}
}

void SyncCommandList::Execute()
{
	SyncCommandState scs(16384);
	scs.frameIndex = frameIndex;
	scs.client = client;

	scs.flushBuffer = [this, &scs]()
	{
		FlushBuffer(scs.cloneBuffer, HashRageString("msgPackedClones"), frameIndex, client);
	};

	scs.maybeFlushBuffer = [this, &scs]()
	{
		MaybeFlushBuffer(scs.cloneBuffer, HashRageString("msgPackedClones"), frameIndex, client);
	};

	for (auto& cmd : commands)
	{
		cmd(scs);
	}

	scs.flushBuffer();
}

void ServerGameState::Tick(fx::ServerInstanceBase* instance)
{
	{
		std::shared_lock<std::shared_mutex> lock(m_entityListMutex);

		for (auto& entity : m_entityList)
		{
			if (entity)
			{
				entity->frameIndex = m_frameIndex;
			}
		}
	}

	UpdateWorldGrid(instance);

	UpdateEntities();

	// cache entities so we don't have to iterate the concurrent_map for each client
	std::vector<
		std::tuple<
			std::shared_ptr<sync::SyncEntityState>,
			glm::vec3,
			sync::CVehicleGameStateNodeData*,
			std::shared_ptr<fx::Client>
		>
	> relevantEntities;

	{
		std::shared_lock<std::shared_mutex> lock(m_entityListMutex);

		relevantEntities.reserve(m_entityList.size());

		for (auto& entity : m_entityList)
		{
			if (!entity || !entity->syncTree)
			{
				continue;
			}

			float position[3];
			entity->syncTree->GetPosition(position);

			glm::vec3 entityPosition(position[0], position[1], position[2]);

			sync::CVehicleGameStateNodeData* vehicleData = nullptr;

			if (entity->type == sync::NetObjEntityType::Automobile ||
				entity->type == sync::NetObjEntityType::Bike ||
				entity->type == sync::NetObjEntityType::Boat ||
				entity->type == sync::NetObjEntityType::Heli ||
				entity->type == sync::NetObjEntityType::Plane ||
				entity->type == sync::NetObjEntityType::Submarine ||
				entity->type == sync::NetObjEntityType::Trailer ||
				entity->type == sync::NetObjEntityType::Train)
			{
				vehicleData = entity->syncTree->GetVehicleGameState();
			}

			std::shared_ptr<fx::Client> entityClient;

			{
				std::shared_lock<std::shared_mutex> lock(entity->clientMutex);
				entityClient = entity->client.lock();
			}

			relevantEntities.emplace_back(entity, entityPosition, vehicleData, entityClient);
		}
	}

	auto curTime = msec();

	instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&](const std::shared_ptr<fx::Client>& clientRef)
	{
		// get our own pointer ownership
		auto client = clientRef;

		if (!client)
		{
			return;
		}

		if (client->GetSlotId() == -1)
		{
			return;
		}

		{
			auto[data, lock] = GetClientData(this, client);

			if (!data->playerId)
			{
				return;
			}
		}

		bool shouldSkip = false;

		{
			auto [ clientData, clientDataLock ] = GetClientData(this, client);
			auto& ackPacket = clientData->ackBuffer;

			// any ACKs to send?
			clientData->FlushAcks();

			if (clientData->syncing)
			{
				shouldSkip = true;
			}
			else
			{
				clientData->syncing = true;
			}
		}

		if (shouldSkip)
		{
			return;
		}

		auto scl = std::make_shared<SyncCommandList>();
		scl->client = client;
		scl->frameIndex = m_frameIndex;

		uint64_t time = curTime.count();

		scl->commands.emplace_back([time](SyncCommandState& state)
		{
			state.cloneBuffer.Write(3, 5);
			state.cloneBuffer.Write(32, uint32_t(time & 0xFFFFFFFF));
			state.cloneBuffer.Write(32, uint32_t((time >> 32) & 0xFFFFFFFF));
			state.maybeFlushBuffer();
		});

		if (!client)
		{
			return;
		}

		auto enPeer = gscomms_get_peer(client->GetPeer());

		auto resendDelay = 0ms;

		if (enPeer.GetRef())
		{
			resendDelay = std::chrono::milliseconds(std::max(int(1), int(enPeer->GetPing() * 3) - int(enPeer->GetPingVariance())));
		}

		int numCreates = 0, numSyncs = 0, numSkips = 0;

		std::shared_ptr<sync::SyncEntityState> playerEntity;

		{
			auto data = GetClientDataUnlocked(this, client);
			auto entityRef = data->playerEntity;

			playerEntity = entityRef.lock();
		}

		glm::vec3 playerPos;

		if (playerEntity)
		{
			playerPos = GetPlayerFocusPos(playerEntity);
		}

		auto clientDataUnlocked = GetClientDataUnlocked(this, client);
		auto slotId = client->GetSlotId();

		for (const auto& entityTuple : relevantEntities)
		{
			const auto& [entity, entityPos, vehicleData, entityClient] = entityTuple;

			if (!client)
			{
				return;
			}

			if (!entityClient)
			{
				continue; 
			}

			bool hasCreated = entity->ackedCreation.test(slotId);

			bool shouldBeCreated = (g_oneSyncCulling->GetValue()) ? false : true;

			// players should always have their own entities
			if (client->GetNetId() == entityClient->GetNetId())
			{
				shouldBeCreated = true;
			}

			if (!shouldBeCreated)
			{
				if (playerEntity)
				{
					float diffX = entityPos.x - playerPos.x;
					float diffY = entityPos.y - playerPos.y;

					float distSquared = (diffX * diffX) + (diffY * diffY);

					// #TODO1S: figure out a good value for this
					if (distSquared < (350.0f * 350.0f))
					{
						shouldBeCreated = true;
					}
				}
				else
				{
					// can't really say otherwise if the player entity doesn't exist
					shouldBeCreated = true;
				}
			}

			// #TODO1S: improve logic for what should and shouldn't exist based on game code
			if (!shouldBeCreated)
			{
				if (entity->type == sync::NetObjEntityType::Player)
				{
					shouldBeCreated = true;
				}
				else if (entity->type == sync::NetObjEntityType::Automobile ||
					entity->type == sync::NetObjEntityType::Bike ||
					entity->type == sync::NetObjEntityType::Boat ||
					entity->type == sync::NetObjEntityType::Heli ||
					entity->type == sync::NetObjEntityType::Plane ||
					entity->type == sync::NetObjEntityType::Submarine ||
					entity->type == sync::NetObjEntityType::Trailer ||
					entity->type == sync::NetObjEntityType::Train)
				{
					if (vehicleData)
					{
						if (vehicleData->playerOccupants.any())
						{
							shouldBeCreated = true;
						}
					}
				}
			}

			auto syncDelay = 50ms;

			if (g_oneSyncRadiusFrequency->GetValue())
			{
				const auto& position = entityPos;

				if (entity->syncTree)
				{
					// get an average radius from a list of type radii (until we store modelinfo somewhere)
					float objRadius = 5.0f;

					switch (entity->type)
					{
					case sync::NetObjEntityType::Ped:
					case sync::NetObjEntityType::Player:
						objRadius = 2.5f;
						break;
					case sync::NetObjEntityType::Heli:
					case sync::NetObjEntityType::Boat:
					case sync::NetObjEntityType::Plane:
						objRadius = 15.0f;
						break;
					}

					if (!IsInFrustum(position, objRadius, clientDataUnlocked->viewMatrix))
					{
						syncDelay = 150ms;
					}

					if (playerEntity)
					{
						auto dist = glm::distance2(position, playerPos);

						if (dist > 500.0f * 500.0f)
						{
							syncDelay = 500ms;
						}
						else if (dist > 250.0f * 250.0f)
						{
							syncDelay = 250ms;
						}
					}
				}
			}

			if (shouldBeCreated)
			{
				// default to it being a sync
				int syncType = 2;

				if (!hasCreated || entity->didDeletion.test(slotId))
				{
					GS_LOG("Tick: %screating object %d for %d\n", (hasCreated) ? "re" : "", entity->handle & 0xFFFF, client->GetNetId());

					// make it a create
					syncType = 1;
				}

				bool shouldSend = true;

				// TODO: proper hazards
				if (slotId == -1)
				{
					break;
				}

				auto lastResend = entity->lastResends[slotId];
				auto lastTime = (curTime - lastResend);

				if (lastResend != 0ms && lastTime < resendDelay)
				{
					GS_LOG("%s: skipping resend for object %d (resend delay %dms, last resend %d)\n", __func__, entity->handle & 0xFFFF, resendDelay.count(), lastTime.count());
					shouldSend = false;
				}

				if (syncType == 2 && shouldSend)
				{
					auto lastSync = entity->lastSyncs[slotId];
					auto lastTime = (curTime - lastSync);

					if (lastTime < syncDelay)
					{
						GS_LOG("%s: skipping sync for object %d (sync delay %dms, last sync %d)\n", __func__, entity->handle & 0xFFFF, syncDelay.count(), lastTime.count());

						shouldSend = false;
					}
				}

				if (shouldSend)
				{
					scl->commands.emplace_back([
						this,
						entity = std::move(entity),
						entityClient = std::move(entityClient),
						resendDelay,
						syncDelay,
						syncType,
						curTime
					] (SyncCommandState& cmdState)
					{
						if (!entity)
						{
							return;
						}

						if (cmdState.client->GetSlotId() == -1)
						{
							return;
						}

						// create a buffer once (per thread) to save allocations
						static thread_local rl::MessageBuffer mb(1200);
						mb.SetCurrentBit(0);

						sync::SyncUnparseState state(mb);
						state.syncType = syncType;
						state.client = cmdState.client;

						bool wroteData = entity->syncTree->Unparse(state);

						if (wroteData)
						{
							auto len = (state.buffer.GetCurrentBit() / 8) + 1;

							if (len > 4096)
							{
								return;
							}

							auto startBit = cmdState.cloneBuffer.GetCurrentBit();

							{
								auto[clientData, lock] = GetClientData(this, cmdState.client);
								clientData->idsForGameState.emplace(cmdState.frameIndex, entity->handle & 0xFFFF);
							}

							cmdState.cloneBuffer.Write(3, syncType);
							cmdState.cloneBuffer.Write(13, entity->handle & 0xFFFF);
							cmdState.cloneBuffer.Write(16, entityClient->GetNetId()); // TODO: replace with slotId

							if (syncType == 1)
							{
								cmdState.cloneBuffer.Write(4, (uint8_t)entity->type);
							}

							cmdState.cloneBuffer.Write<uint32_t>(32, entity->timestamp);

							cmdState.cloneBuffer.Write(12, len);
							
							if (!cmdState.cloneBuffer.WriteBits(state.buffer.GetBuffer().data(), len * 8))
							{
								cmdState.cloneBuffer.SetCurrentBit(startBit);

								// force a buffer flush, we're oversize
								cmdState.flushBuffer();
							}
							else
							{
								auto slotId = cmdState.client->GetSlotId();

								if (slotId == -1)
								{
									return;
								}

								entity->lastSyncs[slotId] = entity->lastResends[slotId] = curTime;
							}

							cmdState.maybeFlushBuffer();
						}
					});
				}
			}
			else
			{
				if (hasCreated)
				{
					GS_LOG("Tick: distance-culling object %d for %d\n", entity->handle & 0xFFFF, client->GetNetId());

					{
						auto [clientData, clientDataLock] = GetClientData(this, client);
						clientData->pendingRemovals.set(entity->handle & 0xFFFF);
					}

					// unacknowledge creation
					entity->ackedCreation.reset(slotId);
					entity->didDeletion.set(slotId);
				}
			}
		}

		{
			scl->commands.emplace_back([
				this
			] (SyncCommandState& cmdState)
			{
				// NOTE: this is a thread hazard, but generally it doesn't matter if the bitset is inconsistent here
				// all that'll happen is we'll send a removal _later_, or send _duplicates_, both of which are typically fine.
				auto clientData = GetClientDataUnlocked(this, cmdState.client);

				for (uint16_t i = 0; i < MaxObjectId; i++)
				{
					if (clientData->pendingRemovals.test(i))
					{
						cmdState.cloneBuffer.Write(3, 3);
						cmdState.cloneBuffer.Write(13, i);
						cmdState.maybeFlushBuffer();
					}
				}
			});
		}

		if (!m_tg->tryPost([this, scl]()
		{
			scl->Execute();

			auto [clientData, clientDataLock] = GetClientData(this, scl->client);
			clientData->syncing = false;
		}))
		{
#ifndef _MSC_VER
			GS_LOG("Thread pool full?\n", 0);
#else
			GS_LOG("Thread pool full?\n");
#endif
		}

		GS_LOG("Tick: cl %d: %d cr, %d sy, %d sk\n", client->GetNetId(), numCreates, numSyncs, numSkips);

		{
			auto[clientData, clientDataLock] = GetClientData(this, client);

			// since this runs every frame, we can safely assume this will clean things up entirely
			clientData->idsForGameState.erase(m_frameIndex - 100);
		}
	});

	++m_frameIndex;
}

void ServerGameState::OnCloneRemove(const std::shared_ptr<sync::SyncEntityState>& entity, const std::function<void()>& doRemove)
{
	// trigger a clone removal event
	gscomms_execute_callback_on_main_thread([this, entity, doRemove]()
	{
		auto evComponent = m_instance->GetComponent<fx::ResourceManager>()->GetComponent<fx::ResourceEventManagerComponent>();
		evComponent->TriggerEvent2("entityRemoved", { }, MakeScriptHandle(entity));

		gscomms_execute_callback_on_net_thread(doRemove);
	});

	// remove vehicle occupants
	if (entity->type == sync::NetObjEntityType::Ped ||
		entity->type == sync::NetObjEntityType::Player)
	{
		auto pedHandle = entity->handle & 0xFFFF;
		auto vehicleData = entity->syncTree->GetPedGameState();

		if (vehicleData)
		{
			auto curVehicle = (vehicleData->curVehicle != -1) ? GetEntity(0, vehicleData->curVehicle) : nullptr;
			auto curVehicleData = (curVehicle && curVehicle->syncTree) ? curVehicle->syncTree->GetVehicleGameState() : nullptr;

			if (curVehicleData && curVehicleData->occupants[vehicleData->curVehicleSeat] == pedHandle)
			{
				curVehicleData->occupants[vehicleData->curVehicleSeat] = 0;
				curVehicleData->playerOccupants.reset(vehicleData->curVehicleSeat);
			}
		}
	}

	auto objectId = entity->handle & 0xFFFF;
	bool stolen = false;

	{
		std::unique_lock<std::mutex> lock(m_objectIdsMutex);
		if (m_objectIdsStolen.test(objectId))
		{
			stolen = true;

			m_objectIdsSent.reset(objectId);
			m_objectIdsStolen.reset(objectId);
		}
	}

	if (stolen)
	{
		std::shared_ptr<fx::Client> clientRef;
		
		{
			std::shared_lock<std::shared_mutex> lock(entity->clientMutex);
			clientRef = entity->client.lock();
		}

		if (clientRef)
		{
			auto[clientData, lock] = GetClientData(this, clientRef);
			clientData->objectIds.erase(objectId);
		}
	}
}

void ServerGameState::UpdateEntities()
{
	std::shared_lock<std::shared_mutex> lock(m_entityListMutex);

	for (auto& entity : m_entityList)
	{
		if (!entity || !entity->syncTree)
		{
			continue;
		}

		// update client camera
		if (entity->type == sync::NetObjEntityType::Player)
		{
			std::shared_ptr<fx::Client> client;

			{
				std::shared_lock<std::shared_mutex> lock(entity->clientMutex);
				client = entity->client.lock();
			}

			if (client)
			{
				float playerPos[3];
				entity->syncTree->GetPosition(playerPos);

				auto camData = entity->syncTree->GetPlayerCamera();

				if (camData)
				{
					glm::vec3 camTranslate;

					switch (camData->camMode)
					{
					case 0:
					default:
						camTranslate = { playerPos[0], playerPos[1], playerPos[2] };
						break;
					case 1:
						camTranslate = { camData->freeCamPosX, camData->freeCamPosY, camData->freeCamPosZ };
						break;
					case 2:
						camTranslate = { playerPos[0] + camData->camOffX, playerPos[1] + camData->camOffY, playerPos[2] + camData->camOffZ };
						break;
					}

					glm::vec3 camRotation{ camData->cameraX, 0.0f, camData->cameraZ };
					auto camQuat = glm::quat{ camRotation };
					auto rot = glm::toMat4(camQuat);

					auto[data, dataLock] = GetClientData(this, client);
					data->viewMatrix = glm::inverse(glm::translate(glm::identity<glm::mat4>(), camTranslate) * rot);
				}
			}
		}

		// update vehicle seats, if it's a ped
		if (entity->type == sync::NetObjEntityType::Ped ||
			entity->type == sync::NetObjEntityType::Player)
		{
			auto pedHandle = entity->handle & 0xFFFF;
			auto vehicleData = entity->syncTree->GetPedGameState();

			if (vehicleData)
			{
				if (vehicleData->lastVehicle != vehicleData->curVehicle || vehicleData->lastVehicleSeat != vehicleData->curVehicleSeat)
				{
					auto lastVehicle = (vehicleData->lastVehicle != -1) ? GetEntity(0, vehicleData->lastVehicle) : nullptr;
					auto curVehicle = (vehicleData->curVehicle != -1) ? GetEntity(0, vehicleData->curVehicle) : nullptr;

					auto lastVehicleData = (lastVehicle && lastVehicle->syncTree) ? lastVehicle->syncTree->GetVehicleGameState() : nullptr;
					auto curVehicleData = (curVehicle && curVehicle->syncTree) ? curVehicle->syncTree->GetVehicleGameState() : nullptr;

					if (lastVehicleData && lastVehicleData->occupants[vehicleData->lastVehicleSeat] == pedHandle)
					{
						lastVehicleData->occupants[vehicleData->lastVehicleSeat] = 0;
						lastVehicleData->playerOccupants.reset(vehicleData->lastVehicleSeat);
					}

					if (curVehicleData && curVehicleData->occupants[vehicleData->curVehicleSeat] == 0)
					{
						curVehicleData->occupants[vehicleData->curVehicleSeat] = pedHandle;

						if (entity->type == sync::NetObjEntityType::Player)
						{
							curVehicleData->playerOccupants.set(vehicleData->curVehicleSeat);
						}
					}

					vehicleData->lastVehicle = vehicleData->curVehicle;
					vehicleData->lastVehicleSeat = vehicleData->curVehicleSeat;
				}
			}
		}
	}
}

void ServerGameState::SendWorldGrid(void* entry /* = nullptr */, const std::shared_ptr<fx::Client>& client /* = */ )
{
	net::Buffer msg;
	msg.Write<uint32_t>(HashRageString("msgWorldGrid"));
	
	uint16_t base = 0;
	uint16_t length = sizeof(m_worldGrid);

	if (entry)
	{ 
		base = ((WorldGridEntry*)entry - &m_worldGrid[0].entries[0]) * sizeof(WorldGridEntry);
		length = sizeof(WorldGridEntry);
	}

	msg.Write<uint16_t>(base);
	msg.Write<uint16_t>(length);

	msg.Write(reinterpret_cast<char*>(m_worldGrid) + base, length);

	if (client)
	{
		client->SendPacket(1, msg, NetPacketType_ReliableReplayed);
	}
	else
	{
		m_instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&msg](const std::shared_ptr<fx::Client>& client)
		{
			client->SendPacket(1, msg, NetPacketType_ReliableReplayed);
		});
	}
}

void ServerGameState::UpdateWorldGrid(fx::ServerInstanceBase* instance)
{
	instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&](const std::shared_ptr<fx::Client>& client)
	{
		if (client->GetSlotId() == -1)
		{
			return;
		}

		std::weak_ptr<sync::SyncEntityState> entityRef;

		{
			auto[data, lock] = GetClientData(this, client);
			entityRef = data->playerEntity;
		}

		auto playerEntity = entityRef.lock();

		if (!playerEntity)
		{
			return;
		}

		auto pos = GetPlayerFocusPos(playerEntity);

		int minSectorX = std::max((pos.x - 149.0f) + 8192.0f, 0.0f) / 75;
		int maxSectorX = std::max((pos.x + 149.0f) + 8192.0f, 0.0f) / 75;
		int minSectorY = std::max((pos.y - 149.0f) + 8192.0f, 0.0f) / 75;
		int maxSectorY = std::max((pos.y + 149.0f) + 8192.0f, 0.0f) / 75;

		auto slotID = client->GetSlotId();

		WorldGridState* gridState = &m_worldGrid[slotID];

		// disown any grid entries that aren't near us anymore
		for (auto& entry : gridState->entries)
		{
			if (entry.slotID != 0xFF)
			{
				if (entry.sectorX < (minSectorX - 1) || entry.sectorX >= (maxSectorX + 1) ||
					entry.sectorY < (minSectorY - 1) || entry.sectorY >= (maxSectorY + 1))
				{
					if (m_worldGridAccel.slots[entry.sectorX][entry.sectorY] == slotID)
					{
						m_worldGridAccel.slots[entry.sectorX][entry.sectorY] = 0xFF;
					}

					entry.sectorX = 0;
					entry.sectorY = 0;
					entry.slotID = -1;

					SendWorldGrid(&entry);
				}
			}
		}

		for (int x = minSectorX; x <= maxSectorX; x++)
		{
			for (int y = minSectorY; y <= maxSectorY; y++)
			{
				// find if this x/y is owned by someone already
				bool found = (m_worldGridAccel.slots[x][y] != 0xFF);

				// is it free?
				if (!found)
				{
					// time to have some fun!

					// find a free entry slot
					for (auto& entry : gridState->entries)
					{
						if (entry.slotID == 0xFF)
						{
							// and take it
							entry.sectorX = x;
							entry.sectorY = y;
							entry.slotID = slotID;

							m_worldGridAccel.slots[x][y] = slotID;

							SendWorldGrid(&entry);

							break;
						}
					}
				}
			}
		}
	});
}

void ServerGameState::ReassignEntity(uint32_t entityHandle, const std::shared_ptr<fx::Client>& targetClient)
{
	auto entity = GetEntity(0, entityHandle & 0xFFFF);

	if (!entity)
	{
		return;
	}

	std::weak_ptr<fx::Client> oldClient;

	{
		std::unique_lock<std::shared_mutex> clientLock(entity->clientMutex);

		oldClient = entity->client;
		entity->client = targetClient;
	}

	{
		auto oldClientRef = oldClient.lock();

		GS_LOG("%s: obj id %d, old client %d, new client %d\n", __func__, entityHandle & 0xFFFF, (!oldClientRef) ? -1 : oldClientRef->GetNetId(), targetClient->GetNetId());

		if (oldClientRef)
		{
			auto[sourceData, lock] = GetClientData(this, oldClientRef);
			sourceData->objectIds.erase(entityHandle & 0xFFFF);
		}
	}

	// #TODO1S: reassignment should also send a create if the player was out of focus area
	{
		auto [ targetData, lock ] = GetClientData(this, targetClient);
		targetData->objectIds.insert(entityHandle & 0xFFFF);
	}

	// when deleted, we want to make this object ID return to the global pool, not to the player who last owned it
	// therefore, mark it as stolen
	{
		std::unique_lock<std::mutex> lock(m_objectIdsMutex);
		m_objectIdsStolen.set(entityHandle & 0xFFFF);
	}

	// allow this client to be synced instantly again so clients are aware of ownership changes as soon as possible
	entity->lastResends = {};
	entity->lastSyncs = {};

	entity->syncTree->Visit([this](sync::NodeBase& node)
	{
		node.frameIndex = m_frameIndex + 1;
		node.ackedPlayers.reset();

		return true;
	});
}

void ServerGameState::HandleClientDrop(const std::shared_ptr<fx::Client>& client)
{
	if (!g_oneSyncVar->GetValue())
	{
		return;
	}

	auto clientRegistry = m_instance->GetComponent<fx::ClientRegistry>();

#ifndef _MSC_VER
	GS_LOG("client drop - reassigning\n", 0);
#else
	GS_LOG("client drop - reassigning\n");
#endif

	// clear the player's world grid ownership
	if (auto slotId = client->GetSlotId(); slotId != -1)
	{
		WorldGridState* gridState = &m_worldGrid[slotId];

		for (auto& entry : gridState->entries)
		{
			if (m_worldGridAccel.slots[entry.sectorX][entry.sectorY] == slotId)
			{
				m_worldGridAccel.slots[entry.sectorX][entry.sectorY] = 0xFF;
			}

			entry.slotID = -1;
			entry.sectorX = 0;
			entry.sectorY = 0;

			SendWorldGrid(&entry);
		}
	}

	std::set<uint32_t> toErase;

	{
		std::shared_lock<std::shared_mutex> lock(m_entityListMutex);

		for (auto& entity : m_entityList)
		{
			if (!entity || !entity->syncTree)
			{
				continue;
			}

			bool hasClient = true;

			{
				std::shared_lock<std::shared_mutex> clientLock(entity->clientMutex);

				auto entityClient = entity->client.lock();

				if (!entityClient)
				{
					hasClient = false;
				}
				else if (entityClient->GetNetId() == client->GetNetId())
				{
					hasClient = false;
				}
			}

			if (!hasClient)
			{
				float posX = entity->GetData("posX", 0.0f);
				float posY = entity->GetData("posY", 0.0f);
				float posZ = entity->GetData("posZ", 0.0f);

				std::vector<std::tuple<float, std::shared_ptr<fx::Client>>> candidates;

				clientRegistry->ForAllClients([this, &candidates, &client, posX, posY, posZ](const std::shared_ptr<fx::Client>& tgtClient)
				{
					if (tgtClient == client)
					{
						return;
					}

					if (tgtClient->GetSlotId() == 0xFFFFFFFF)
					{
						return;
					}

					float distance = std::numeric_limits<float>::max();

					try
					{
						std::weak_ptr<sync::SyncEntityState> entityRef;

						{
							auto[data, lock] = GetClientData(this, tgtClient);
							entityRef = data->playerEntity;
						}

						auto playerEntity = entityRef.lock();

						if (playerEntity)
						{
							auto tgt = GetPlayerFocusPos(playerEntity);

							if (posX != 0.0f)
							{
								float deltaX = (tgt.x - posX);
								float deltaY = (tgt.y - posY);
								float deltaZ = (tgt.z - posZ);

								distance = (deltaX * deltaX) + (deltaY * deltaY) + (deltaZ * deltaZ);
							}
						}
					}
					catch (std::bad_any_cast&)
					{

					}

					candidates.emplace_back(distance, tgtClient);
				});

				std::sort(candidates.begin(), candidates.end());

				if (entity->type == sync::NetObjEntityType::Player)
				{
					candidates.clear();
				}

				if (candidates.empty() || // no candidate?
					std::get<float>(candidates[0]) >= (300.0f * 300.0f)) // closest candidate beyond distance culling range?
				{
					GS_LOG("no candidates for entity %d, deleting\n", entity->handle);

					toErase.insert(entity->handle);
				}
				else
				{
					GS_LOG("reassigning entity %d from %s to %s\n", entity->handle, client->GetName(), std::get<1>(candidates[0])->GetName());

					ReassignEntity(entity->handle, std::get<1>(candidates[0]));
				}
			}
		}
	}

	// here temporarily, needs to be unified with ProcessCloneRemove
	for (auto& set : toErase)
	{
		RemoveClone(client, set & 0xFFFF);
	}

	{
		// remove object IDs from sent map
		auto [ data, lock ] = GetClientData(this, client);

		std::unique_lock<std::mutex> objectIdsLock(m_objectIdsMutex);

		for (auto& objectId : data->objectIds)
		{
			m_objectIdsSent.reset(objectId);
		}
	}

	// remove ACKs for this client
	if (client->GetSlotId() != 0xFFFFFFFF)
	{
		std::shared_lock<std::shared_mutex> lock(m_entityListMutex);

		for (auto& entity : m_entityList)
		{
			if (entity && entity->syncTree)
			{
				entity->ackedCreation.reset(client->GetSlotId());

				entity->syncTree->Visit([&client](sync::NodeBase& node)
				{
					node.ackedPlayers.reset(client->GetSlotId());

					return true;
				});
			}
		}
	}
}

void ServerGameState::ProcessCloneCreate(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, AckPacketWrapper& ackPacket)
{
	uint16_t objectId = 0;
	ProcessClonePacket(client, inPacket, 1, &objectId);

	{
		std::unique_lock<std::mutex> objectIdsLock(m_objectIdsMutex);
		m_objectIdsUsed.set(objectId);
	}

	ackPacket.Write(3, 1);
	ackPacket.Write(13, objectId);
	ackPacket.flush();

	GS_LOG("%s: cl %d, id %d\n", __func__, client->GetNetId(), objectId);
}

void ServerGameState::ProcessCloneSync(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, AckPacketWrapper& ackPacket)
{
	uint16_t objectId = 0;
	ProcessClonePacket(client, inPacket, 2, &objectId);

	ackPacket.Write(3, 2);
	ackPacket.Write(13, objectId);
	ackPacket.flush();

	GS_LOG("%s: cl %d, id %d\n", __func__, client->GetNetId(), objectId);
}

void ServerGameState::ProcessCloneTakeover(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket)
{
	auto clientId = inPacket.Read<uint16_t>(16);
	auto playerId = 0;
	auto objectId = inPacket.Read<uint16_t>(13);

	auto entity = GetEntity(0, objectId);

	if (entity)
	{
		auto tgtCl = (clientId != 0) ? m_instance->GetComponent<fx::ClientRegistry>()->GetClientByNetID(clientId) : client;

		if (!tgtCl)
		{
			return;
		}

		// don't do duplicate migrations
		{
			std::shared_lock<std::shared_mutex> clientLock(entity->clientMutex);

			auto entityClient = entity->client.lock();

			if (entityClient && entityClient->GetNetId() == tgtCl->GetNetId())
			{
				return;
			}

			if (entityClient && entityClient->GetNetId() != client->GetNetId())
			{
				GS_LOG("%s: trying to send object %d from %s to %s, but the sender is %s. Rejecting.\n", __func__, objectId, (!entityClient) ? "null?" : entityClient->GetName(), tgtCl->GetName(), client->GetName());
				return;
			}

			GS_LOG("%s: migrating object %d from %s to %s\n", __func__, objectId, (!entityClient) ? "null?" : entityClient->GetName(), tgtCl->GetName());
		}

		if (!entity || !entity->syncTree)
		{
			return;
		}

		ReassignEntity(entity->handle, tgtCl);
	}
}

void ServerGameState::ProcessCloneRemove(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, AckPacketWrapper& ackPacket)
{
	auto playerId = 0;
	auto objectId = inPacket.Read<uint16_t>(13);

	// ack remove no matter if we accept it
	ackPacket.Write(3, 3);
	ackPacket.Write(13, objectId);
	ackPacket.flush();

	// TODO: verify ownership
	auto entity = GetEntity(0, objectId);

	if (entity)
	{
		std::shared_lock<std::shared_mutex> clientLock(entity->clientMutex);

		auto entityClient = entity->client.lock();

		if (entityClient)
		{
			if (client->GetNetId() != entityClient->GetNetId())
			{
				GS_LOG("%s: wrong owner (%d)\n", __func__, objectId);

				return;
			}
		}
	}

	RemoveClone(client, objectId);
}

void ServerGameState::RemoveClone(const std::shared_ptr<Client>& client, uint16_t objectId)
{
	GS_LOG("%s: deleting object %d %d\n", __func__, client->GetNetId(), objectId);

	// defer deletion of the object so script has time to do things
	auto continueCloneRemoval = [this, objectId]()
	{
		{
			std::unique_lock<std::mutex> objectIdsLock(m_objectIdsMutex);
			m_objectIdsUsed.reset(objectId);
		}

		std::unique_lock<std::shared_mutex> entityListLock(m_entityListMutex);

		for (auto it = m_entityList.begin(); it != m_entityList.end(); it++)
		{
			if (((*it)->handle & 0xFFFF) == objectId)
			{
				m_entityList.erase(it);
				break;
			}
		}

		// unset weak pointer, as well
		{
			std::unique_lock<std::mutex> lock(m_entitiesByIdMutex);
			m_entitiesById[objectId].reset();
		}
	};

	{
		std::weak_ptr<sync::SyncEntityState> entity;

		{
			std::unique_lock<std::mutex> lock(m_entitiesByIdMutex);
			entity = m_entitiesById[objectId];
		}

		auto entityRef = entity.lock();

		if (entityRef && !entityRef->deleting)
		{
			entityRef->deleting = true;

			OnCloneRemove(entityRef, continueCloneRemoval);

			m_instance->GetComponent<fx::ClientRegistry>()->ForAllClients([&](const std::shared_ptr<fx::Client>& thisClient)
			{
				auto tgtClient = thisClient;

				if (!tgtClient)
				{
					return;
				}

				if (tgtClient->GetNetId() == client->GetNetId())
				{
					return;
				}

				{
					auto [clientData, lock] = GetClientData(this, tgtClient);
					clientData->pendingRemovals.set(objectId);
				}
			});
		}
	}
}

void ServerGameState::ProcessClonePacket(const std::shared_ptr<fx::Client>& client, rl::MessageBuffer& inPacket, int parsingType, uint16_t* outObjectId)
{
	auto playerId = 0;
	auto objectId = inPacket.Read<uint16_t>(13);
	//auto objectType = (sync::NetObjEntityType)inPacket.Read<uint8_t>();
	//auto timestamp = inPacket.Read<int32_t>();

	auto objectType = sync::NetObjEntityType::Train;

	if (parsingType == 1)
	{
		objectType = (sync::NetObjEntityType)inPacket.Read<uint8_t>(4);
	}

	auto length = inPacket.Read<uint16_t>(12);

	uint32_t timestamp = 0;

	if (auto tsData = client->GetData("syncTs"); tsData.has_value())
	{
		timestamp = std::any_cast<uint32_t>(tsData);
	}

	if (!client->GetData("timestamp").has_value())
	{
		client->SetData("timestamp", int64_t(timestamp));
	}

	// move this back down under
	{
		auto [data, lock] = GetClientData(this, client);
		data->playerId = playerId;
	}

	std::vector<uint8_t> bitBytes(length);
	inPacket.ReadBits(&bitBytes[0], bitBytes.size() * 8);

	auto entity = GetEntity(playerId, objectId);

	bool createdHere = false;

	bool validEntity = false;

	{
		if (entity)
		{
			std::shared_lock<std::shared_mutex> lock(entity->clientMutex);

			validEntity = !entity->client.expired();
		}
	}

	if (parsingType == 1)
	{
		if (!validEntity)
		{
			entity = std::make_shared<sync::SyncEntityState>();
			entity->client = client;
			entity->type = objectType;
			entity->guid = nullptr;
			entity->frameIndex = m_frameIndex;
			entity->lastFrameIndex = 0;
			entity->handle = MakeEntityHandle(playerId, objectId);

			entity->syncTree = MakeSyncTree(objectType);

			{
				std::unique_lock<std::shared_mutex> entityListLock(m_entityListMutex);

				m_entityList.push_back(entity);
			}

			createdHere = true;

			{
				std::unique_lock<std::mutex> lock(m_entitiesByIdMutex);

				std::weak_ptr<sync::SyncEntityState> weakEntity(entity);
				m_entitiesById[objectId].swap(weakEntity);
			}
		}
		else // duplicate create? that's not supposed to happen
		{
			std::shared_lock<std::shared_mutex> lock(entity->clientMutex);

			auto lcl = entity->client.lock();

			if (objectType != entity->type)
			{
				GS_LOG("%s: client %d %s tried to create entity %d (type %d), but this is already owned by %d %s (type %d). bad!\n",
					__func__,
					client->GetNetId(),
					client->GetName(),
					objectId,
					(int)objectType,
					(lcl) ? lcl->GetNetId() : -1,
					(lcl) ? lcl->GetName() : "(null)",
					(int)entity->type);

				/*trace("%s: client %d %s tried to create entity %d (type %d), but this is already owned by %d %s (type %d). bad!\n",
					__func__,
					client->GetNetId(),
					client->GetName(),
					objectId,
					(int)objectType,
					(lcl) ? lcl->GetNetId() : -1,
					(lcl) ? lcl->GetName() : "(null)",
					(int)entity->type);*/
			}

			return;
		}
	}
	else if (!validEntity)
	{
		GS_LOG("%s: wrong entity (%d)!\n", __func__, objectId);

		return;
	}

	entity->didDeletion.reset(client->GetSlotId());
	entity->ackedCreation.set(client->GetSlotId());

	std::shared_ptr<fx::Client> entityClient;

	{
		std::shared_lock<std::shared_mutex> lock(entity->clientMutex);
		entityClient = entity->client.lock();
	}

	if (!entityClient)
	{
		return;
	}

	if (entityClient->GetNetId() != client->GetNetId())
	{
		GS_LOG("%s: wrong owner (%d)!\n", __func__, objectId);

		return;
	}

	entity->timestamp = timestamp;

	auto state = sync::SyncParseState{ { bitBytes }, parsingType, 0, entity, m_frameIndex };

	if (entity->syncTree)
	{
		entity->syncTree->Parse(state);

		// reset resends to 0
		entity->lastResends = {};

		if (parsingType == 1)
		{
			entity->syncTree->Visit([](sync::NodeBase& node)
			{
				node.ackedPlayers.reset();

				return true;
			});
		}
	}

	switch (entity->type)
	{
		case sync::NetObjEntityType::Player:
		{
			auto[data, lock] = GetClientData(this, client);
			auto entityRef = data->playerEntity;

			if (entityRef.expired())
			{
				SendWorldGrid(nullptr, client);
			}

			data->playerEntity = entity;

			client->SetData("playerEntity", MakeScriptHandle(entity));

			break;
		}
	}

	if (outObjectId)
	{
		*outObjectId = objectId;
	}

	// trigger a clone creation event
	if (createdHere)
	{
		auto evComponent = m_instance->GetComponent<fx::ResourceManager>()->GetComponent<fx::ResourceEventManagerComponent>();
		evComponent->QueueEvent2("entityCreated", { }, MakeScriptHandle(entity));

		// update all clients' lists so the system knows that this entity is valid and should not be deleted anymore
		// (otherwise embarrassing things happen like a new player's ped having the same object ID as a pending-removed entity, and the game trying to remove it)
		m_instance->GetComponent<fx::ClientRegistry>()->ForAllClients([this, objectId](const std::shared_ptr<fx::Client> & client)
		{
			auto [clientData, lock] = GetClientData(this, client);
			clientData->pendingRemovals.reset(objectId);
		});
	}
}

static std::tuple<std::optional<net::Buffer>, uint32_t> UncompressClonePacket(const std::vector<uint8_t>& packetData)
{
	net::Buffer readBuffer(packetData);
	auto type = readBuffer.Read<uint32_t>();

	if (type != HashString("netClones") && type != HashString("netAcks"))
	{
		return { std::optional<net::Buffer>{}, type };
	}

	uint8_t bufferData[16384] = { 0 };
	int bufferLength = LZ4_decompress_safe(reinterpret_cast<const char*>(&readBuffer.GetData()[4]), reinterpret_cast<char*>(bufferData), readBuffer.GetRemainingBytes(), sizeof(bufferData));

	if (bufferLength <= 0)
	{
		return { std::optional<net::Buffer>{}, type };
	}

	return { { {bufferData, size_t(bufferLength)} }, type };
}

void ServerGameState::ParseGameStatePacket(const std::shared_ptr<fx::Client>& client, const std::vector<uint8_t>& packetData)
{
	if (!g_oneSyncVar->GetValue())
	{
		return;
	}

	auto [packet, type] = UncompressClonePacket(packetData);

	if (!packet)
	{
		return;
	}

	switch (type)
	{
	case HashString("netClones"):
		ParseClonePacket(client, *packet);
		break;
	case HashString("netAcks"):
		ParseAckPacket(client, *packet);
		break;
	}
}

void ServerGameState::ParseAckPacket(const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
{
	rl::MessageBuffer msgBuf(buffer.GetData().data() + buffer.GetCurOffset(), buffer.GetRemainingBytes());

	bool end = false;

	while (!msgBuf.IsAtEnd() && !end)
	{
		auto dataType = msgBuf.Read<uint8_t>(3);

		switch (dataType)
		{
		case 1: // clone create
		{
			auto objectId = msgBuf.Read<uint16_t>(13);
			auto entity = GetEntity(0, objectId);

			if (entity && entity->syncTree)
			{
				entity->syncTree->Visit([client](fx::sync::NodeBase & node)
				{
					node.ackedPlayers.set(client->GetSlotId());

					return true;
				});

				entity->didDeletion.reset(client->GetSlotId());
				entity->ackedCreation.set(client->GetSlotId());
			}
		}
		case 3: // clone remove
		{
			auto objectId = msgBuf.Read<uint16_t>(13);

			auto [clientData, lock] = GetClientData(this, client);
			clientData->pendingRemovals.reset(objectId);

			break;
		}
		case 7: // end
			end = true;
			break;
		default:
			end = true;
			break;
		}
	}
}

void ServerGameState::ParseClonePacket(const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
{
	rl::MessageBuffer msgBuf(buffer.GetData().data() + buffer.GetCurOffset(), buffer.GetRemainingBytes());

	rl::MessageBuffer ackPacket;

	{
		auto[clientData, lock] = GetClientData(this, client);

		ackPacket = std::move(clientData->ackBuffer);
	}

	AckPacketWrapper ackPacketWrapper{ ackPacket };
	ackPacketWrapper.flush = [&ackPacket, client]()
	{
		MaybeFlushBuffer(ackPacket, HashRageString("msgPackedAcks"), 0, client);
	};

	uint32_t numCreates = 0, numSyncs = 0, numRemoves = 0;

	bool end = false;
	
	while (!msgBuf.IsAtEnd() && !end)
	{
		auto dataType = msgBuf.Read<uint8_t>(3);

		switch (dataType)
		{
		case 1: // clone create
			ProcessCloneCreate(client, msgBuf, ackPacketWrapper);
			++numCreates;
			break;
		case 2: // clone sync
			ProcessCloneSync(client, msgBuf, ackPacketWrapper);
			++numSyncs;
			break;
		case 3: // clone remove
			ProcessCloneRemove(client, msgBuf, ackPacketWrapper);
			++numRemoves;
			break;
		case 4: // clone takeover
			ProcessCloneTakeover(client, msgBuf);
			break;
		case 5: // set timestamp
		{
			auto newTs = msgBuf.Read<uint32_t>(32);

			// this is the timestamp that the client will use for following acks
			ackPacket.Write(3, 5);
			ackPacket.Write(32, newTs);
			ackPacketWrapper.flush();

			auto oldTs = client->GetData("ackTs");

			if (!oldTs.has_value() || std::any_cast<uint32_t>(oldTs) < newTs)
			{
				client->SetData("ackTs", newTs);
				client->SetData("syncTs", newTs);
			}

			break;
		}
		case 7: // end
			end = true;
			break;
		default:
			end = true;
			break;
		}
	}

	{
		auto [clientData, lock] = GetClientData(this, client);

		clientData->ackBuffer = std::move(ackPacket);
	}
}

void ServerGameState::SendObjectIds(const std::shared_ptr<fx::Client>& client, int numIds)
{
	// first, gather IDs
	std::vector<int> ids;

	{
		auto [data, lock] = GetClientData(this, client);
		std::unique_lock<std::mutex> objectIdsLock(m_objectIdsMutex);

		int id = 1;

		for (int i = 0; i < numIds; i++)
		{
			bool hadId = false;

			for (; id < m_objectIdsSent.size(); id++)
			{
				if (!m_objectIdsSent.test(id) && !m_objectIdsUsed.test(id))
				{
					hadId = true;

					data->objectIds.insert(id);

					ids.push_back(id);
					m_objectIdsSent.set(id);

					break;
				}
			}

			if (!hadId)
			{
				trace("couldn't assign an object id for player!\n");
				break;
			}
		}
	}

	// compress and send

	// adapted from https://stackoverflow.com/a/1081776
	std::vector<std::tuple<int, int>> pairs;

	int last = -1, len = 0;

	for (int i = 0; i < ids.size(); )
	{
		int gap = ids[i] - 2 - last;
		int size = 0;

		while (++i < ids.size() && ids[i] == ids[i - 1] + 1) size++;

		last = ids[i - 1];

		pairs.emplace_back(gap, size);
	}

	net::Buffer outBuffer;
	outBuffer.Write<uint32_t>(HashRageString("msgObjectIds"));
	outBuffer.Write<uint16_t>(pairs.size());

	for (auto& pair : pairs)
	{
		auto [gap, size] = pair;

		outBuffer.Write<uint16_t>(gap);
		outBuffer.Write<uint16_t>(size);
	}

	client->SendPacket(1, outBuffer, NetPacketType_ReliableReplayed);
}

void ServerGameState::AttachToObject(fx::ServerInstanceBase* instance)
{
	m_instance = instance;

	static auto showObjectIdsCommand = instance->AddCommand("onesync_showObjectIds", [this]()
	{
		console::Printf("net", "^2GLOBAL: %d/%d object IDs used/sent (%.2f percent)^7\n", m_objectIdsUsed.count(), m_objectIdsSent.count(), (m_objectIdsUsed.count() / (float)m_objectIdsSent.count()) * 100.0f);
		
		m_instance->GetComponent<fx::ClientRegistry>()->ForAllClients([this](const std::shared_ptr<fx::Client>& client)
		{
			auto [data, lock] = GetClientData(this, client);
			int used = 0;

			{
				std::unique_lock<std::mutex> entityListLock(m_entitiesByIdMutex);

				for (auto object : data->objectIds)
				{
					if (!m_entitiesById[object].expired())
					{
						used++;
					}
				}
			}

			console::Printf("net", "%s^7: %d/%d object IDs used/sent (%.2f percent)\n", client->GetName(), used, data->objectIds.size(), (used / (float)data->objectIds.size()) * 100.0f);
		});

		console::Printf("net", "---------------- END OBJECT ID DUMP ----------------\n");
	});
}
}

#include <ResourceManager.h>
#include <ServerInstanceBase.h>
#include <ServerInstanceBaseRef.h>
#include <ScriptEngine.h>

static InitFunction initFunction([]()
{
	g_scriptHandlePool = new CPool<fx::ScriptGuid>(1500, "fx::ScriptGuid");

	fx::ServerInstanceBase::OnServerCreate.Connect([](fx::ServerInstanceBase* instance)
	{
		g_oneSyncVar = instance->AddVariable<bool>("onesync_enabled", ConVar_ServerInfo, false);
		g_oneSyncCulling = instance->AddVariable<bool>("onesync_distanceCulling", ConVar_None, true);
		g_oneSyncRadiusFrequency = instance->AddVariable<bool>("onesync_radiusFrequency", ConVar_None, true);
		g_oneSyncLogVar = instance->AddVariable<std::string>("onesync_logFile", ConVar_None, "");

		instance->SetComponent(new fx::ServerGameState);

		instance->GetComponent<fx::GameServer>()->OnNetworkTick.Connect([=]()
		{
			if (!g_oneSyncVar->GetValue())
			{
				return;
			}

			instance->GetComponent<fx::ServerGameState>()->Tick(instance);
		});

		auto gameServer = instance->GetComponent<fx::GameServer>();

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("msgNetGameEvent"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			auto targetPlayerCount = buffer.Read<uint8_t>();
			std::vector<uint16_t> targetPlayers(targetPlayerCount);

			if (!buffer.Read(targetPlayers.data(), targetPlayers.size() * sizeof(uint16_t)))
			{
				return;
			}

			net::Buffer netBuffer;
			netBuffer.Write<uint32_t>(HashRageString("msgNetGameEvent"));
			netBuffer.Write<uint16_t>(client->GetNetId());
			buffer.ReadTo(netBuffer, buffer.GetRemainingBytes());

			auto clientRegistry = instance->GetComponent<fx::ClientRegistry>();

			for (uint16_t player : targetPlayers)
			{
				auto targetClient = clientRegistry->GetClientByNetID(player);

				if (targetClient)
				{
					targetClient->SendPacket(1, netBuffer, NetPacketType_Reliable);
				}
			}
		});

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("msgRequestObjectIds"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			instance->GetComponent<fx::ServerGameState>()->SendObjectIds(client, 32);
		});

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("gameStateAck"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			uint64_t frameIndex = buffer.Read<uint64_t>();

			std::unordered_set<uint32_t> ignoreHandles;
			uint8_t ignoreCount = buffer.Read<uint8_t>();

			for (int i = 0; i < ignoreCount; i++)
			{
				ignoreHandles.insert(fx::MakeEntityHandle(0, buffer.Read<uint16_t>()));
			}

			auto sgs = instance->GetComponent<fx::ServerGameState>();

			auto [clientData, lock] = GetClientData(sgs.GetRef(), client);

			{
				for (auto& entityIdPair : fx::GetIteratorView(clientData->idsForGameState.equal_range(frameIndex)))
				{
					auto[_, entityId] = entityIdPair;
					auto entityRef = sgs->GetEntity(0, entityId);

					if (entityRef)
					{
						if (!entityRef->syncTree)
						{
							continue;
						}

						bool hasCreated = entityRef->ackedCreation.test(client->GetSlotId());
						bool hasDeleted = entityRef->didDeletion.test(client->GetSlotId());

						if (!hasCreated || hasDeleted)
						{
							continue;
						}

						if (ignoreHandles.find(entityRef->handle) != ignoreHandles.end())
						{
							continue;
						}

						entityRef->syncTree->Visit([client, frameIndex](fx::sync::NodeBase& node)
						{
							if (node.frameIndex <= frameIndex)
							{
								node.ackedPlayers.set(client->GetSlotId());
							}

							return true;
						});
					}
				}

				clientData->idsForGameState.erase(frameIndex);
			}

			client->SetData("syncFrameIndex", frameIndex);
		});

		gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("msgTimeSyncReq"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			auto reqTime = buffer.Read<uint32_t>();
			auto reqSeq = buffer.Read<uint32_t>();

			net::Buffer netBuffer;
			netBuffer.Write<uint32_t>(HashRageString("msgTimeSync"));
			netBuffer.Write<uint32_t>(reqTime);
			netBuffer.Write<uint32_t>(reqSeq);
			netBuffer.Write<uint32_t>((msec().count()) & 0xFFFFFFFF);

			client->SendPacket(1, netBuffer, NetPacketType_ReliableReplayed);
		});

		// TODO: handle this based on specific nodes sent with a specific ack
		/*gameServer->GetComponent<fx::HandlerMapComponent>()->Add(HashRageString("csack"), [=](const std::shared_ptr<fx::Client>& client, net::Buffer& buffer)
		{
			auto entity = instance->GetComponent<fx::ServerGameState>()->m_entities[fx::MakeEntityHandle(0, buffer.Read<uint16_t>())];

			if (entity)
			{
				auto slotId = client->GetSlotId();

				entity->syncTree->Visit([slotId](fx::sync::NodeBase& node)
				{
					node.ackedPlayers.set(slotId);

					return true;
				});
			}
		});*/
	}, 999999);
});
