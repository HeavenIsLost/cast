/**
 * The Forgotten Server - a free and open-source MMORPG server emulator
 * Copyright (C) 2015  Mark Samman <mark.samman@gmail.com>
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

#include "otpch.h"

#include "protocollogin.h"

#include "outputmessage.h"
#include "connection.h"
#include "rsa.h"
#include "tasks.h"

#include "configmanager.h"
#include "tools.h"
#include "iologindata.h"
#include "ban.h"
#include "game.h"

#include "protocolcaster.h"

extern ConfigManager g_config;
extern Game g_game;

void ProtocolLogin::disconnectClient(const std::string& message, uint16_t version)
{
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		output->addByte(version >= 1076 ? 0x0B : 0x0A);
		output->addString(message);
		OutputMessagePool::getInstance()->send(output);
	}

	getConnection()->close();
}

void ProtocolLogin::getCharacterList(const std::string& accountName, const std::string& password, uint16_t version)
{
	Account account;
	if (!IOLoginData::loginserverAuthentication(accountName, password, account)) {
		disconnectClient("Account name or password is not correct.", version);
		return;
	}

	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		//Update premium days
		Game::updatePremium(account);

		//Add MOTD
		output->addByte(0x14);

		std::ostringstream ss;
		ss << g_game.getMotdNum() << "\n" << g_config.getString(ConfigManager::MOTD);
		output->addString(ss.str());

		//Add session key
		output->addByte(0x28);
		output->addString(accountName + "\n" + password);

		//Add char list
		output->addByte(0x64);

		output->addByte(1); // number of worlds

		output->addByte(0); // world id
		output->addString(g_config.getString(ConfigManager::SERVER_NAME));
		output->addString(g_config.getString(ConfigManager::IP));
		output->add<uint16_t>(g_config.getNumber(ConfigManager::GAME_PORT));
		output->addByte(0);

		uint8_t size = std::min<size_t>(std::numeric_limits<uint8_t>::max(), account.characters.size());
		output->addByte(size);
		for (uint8_t i = 0; i < size; i++) {
			output->addByte(0);
			output->addString(account.characters[i]);
		}

		//Add premium days
		if (g_config.getBoolean(ConfigManager::FREE_PREMIUM)) {
			output->add<uint16_t>(0xFFFF);    //client displays free premium
		} else {
			output->add<uint16_t>(account.premiumDays);
		}

		OutputMessagePool::getInstance()->send(output);
	}

	getConnection()->close();
}


void ProtocolLogin::getCastingStreamsList(const std::string& password, uint16_t version)
{
	const auto& casts = ProtocolCaster::getLiveCasts();
	std::vector<std::pair<uint32_t, std::string>> castList;

	bool havePassword = !password.empty();

	for (const auto& cast : casts) {
		if (havePassword) {
			if (cast.second->isPasswordProtected() && (cast.second->getLiveCastPassword() == password)) {
				castList.push_back(std::make_pair(cast.second->getSpectatorCount(), cast.first->getName()));
			}
		} else {
			castList.push_back(std::make_pair(cast.second->getSpectatorCount(), cast.first->getName()));
		}
	}

	if (castList.size() == 0) {
		if (havePassword) {
			disconnectClient("No cast avaliable with this password.", version);
		} else {
			disconnectClient("No cast avaliable.", version);
		}

		return;
	}

	std::sort(castList.begin(), castList.end(),
		[](const std::pair<uint32_t, std::string>& lhs, const std::pair<uint32_t, std::string>& rhs) {
		return lhs.first > rhs.first;
	});

	//dispatcher thread
	OutputMessage_ptr output = OutputMessagePool::getInstance()->getOutputMessage(this, false);
	if (output) {
		//Add MOTD
		output->addByte(0x14);

		std::ostringstream ss;
		ss << g_game.getMotdNum() << "\n" << g_config.getString(ConfigManager::MOTD);
		output->addString(ss.str());

		//Add session key
		output->addByte(0x28);
		output->addString("\n" + password);

		//Add char list
		output->addByte(0x64);

		//add worlds

		output->addByte(castList.size());

		uint32_t world = 0;

		for (const auto& it : castList) {
			output->addByte(world); // world id

			uint32_t count = it.first;

			std::stringstream ss;

			if (count == 0)
				ss << "no viewer";
			else if (count == 1)
				ss << count << "viewer";
			else
				ss << count << " viewers";

			output->addString(ss.str());
			output->addString(g_config.getString(ConfigManager::IP));
			output->add<uint16_t>(g_config.getNumber(ConfigManager::LIVE_CAST_PORT));
			output->addByte(0);

			world++;
		}

		world = 0;

		output->addByte(castList.size());

		for (const auto& it : castList) {
			output->addByte(world); // world id

			output->addString(it.second);

			world++;
		}

		output->add<uint16_t>(0x0); //The client expects the number of premium days left.
		OutputMessagePool::getInstance()->send(output);
	}
	getConnection()->close();
}

void ProtocolLogin::onRecvFirstMessage(NetworkMessage& msg)
{
	if (g_game.getGameState() == GAME_STATE_SHUTDOWN) {
		getConnection()->close();
		return;
	}

	msg.skipBytes(2); // client OS

	uint16_t version = msg.get<uint16_t>();
	if (version >= 971) {
		msg.skipBytes(17);
	} else {
		msg.skipBytes(12);
	}
	/*
	 * Skipped bytes:
	 * 4 bytes: protocolVersion
	 * 12 bytes: dat, spr, pic signatures (4 bytes each)
	 * 1 byte: 0
	 */

#define dispatchDisconnectClient(err) g_dispatcher.addTask(createTask(std::bind(&ProtocolLogin::disconnectClient, this, err, version)))

	if (version <= 760) {
		dispatchDisconnectClient("Only clients with protocol " CLIENT_VERSION_STR " allowed!");
		return;
	}

	if (!Protocol::RSA_decrypt(msg)) {
		getConnection()->close();
		return;
	}

	uint32_t key[4];
	key[0] = msg.get<uint32_t>();
	key[1] = msg.get<uint32_t>();
	key[2] = msg.get<uint32_t>();
	key[3] = msg.get<uint32_t>();
	enableXTEAEncryption();
	setXTEAKey(key);

	if (version < CLIENT_VERSION_MIN || version > CLIENT_VERSION_MAX) {
		dispatchDisconnectClient("Only clients with protocol " CLIENT_VERSION_STR " allowed!");
		return;
	}

	if (g_game.getGameState() == GAME_STATE_STARTUP) {
		dispatchDisconnectClient("Gameworld is starting up. Please wait.");
		return;
	}

	if (g_game.getGameState() == GAME_STATE_MAINTAIN) {
		dispatchDisconnectClient("Gameworld is under maintenance.\nPlease re-connect in a while.");
		return;
	}

	BanInfo banInfo;
	if (IOBan::isIpBanned(getConnection()->getIP(), banInfo)) {
		if (banInfo.reason.empty()) {
			banInfo.reason = "(none)";
		}

		std::ostringstream ss;
		ss << "Your IP has been banned until " << formatDateShort(banInfo.expiresAt) << " by " << banInfo.bannedBy << ".\n\nReason specified:\n" << banInfo.reason;
		dispatchDisconnectClient(ss.str());
		return;
	}

	std::string accountName = msg.getString();
	std::string password = msg.getString();
	if (accountName.empty()) {
		if (!g_config.getBoolean(ConfigManager::ENABLE_LIVE_CASTING)) {
			dispatchDisconnectClient("Invalid account name.");
		} else {
			g_dispatcher.addTask(createTask(std::bind(&ProtocolLogin::getCastingStreamsList, this, password, version)));
		}
		return;
	}

#undef dispatchDisconnectClient

	g_dispatcher.addTask(createTask(std::bind(&ProtocolLogin::getCharacterList, this, accountName, password, version)));
}
