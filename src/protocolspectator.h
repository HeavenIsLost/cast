/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2014  Mark Samman <mark.samman@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#ifndef FS_PROTOCOLSPECTATOR_H
#define FS_PROTOCOLSPECTATOR_H

#include "protocolgame.h"
#include "protocolcaster.h"

class ProtocolCaster;

class ProtocolSpectator : public ProtocolGame
{
	public:
		static const char* protocol_name() {
			return "spectator protocol";
		}

		ProtocolSpectator(Connection_ptr connection);

		void setSpectatorName(std::string newName) {
			spectatorName = newName;
		}

		const std::string getSpectatorName() {
			return spectatorName;
		}

		void setSpectatorId(uint32_t id) {
			spectatorId = id;
		}

		const uint32_t getSpectatorId() {
			return spectatorId;
		}

		void setPlayer(Player* p) override;

	private:

		ProtocolCaster* client;
		OperatingSystem_t operatingSystem;
		std::string spectatorName;
		uint32_t spectatorId;

		void login(const std::string& liveCastName, const std::string& password);
		void logout();
		
		void disconnectSpectator(const std::string& message);

		void parsePacket(NetworkMessage& msg) override;
		void onRecvFirstMessage(NetworkMessage& msg) override;
		
		void syncChatChannels();

		void syncOpenContainers();
		void sendEmptyTileOnPlayerPos(const Tile* tile, const Position& playerPos);
		
		void releaseProtocol() override;
		void deleteProtocolTask() override;
		
		bool parseCoomand(std::string text);

		void parseSpectatorSay(NetworkMessage& msg);

		void addDummyCreature(NetworkMessage& msg, const uint32_t& creatureID, const Position& playerPos);
		void syncKnownCreatureSets();
};

#endif
