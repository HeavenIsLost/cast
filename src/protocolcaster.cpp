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

#include "otpch.h"

#include "protocolcaster.h"

#include "outputmessage.h"

#include "tile.h"
#include "player.h"
#include "chat.h"

#include "configmanager.h"

#include "game.h"

#include "connection.h"
#include "scheduler.h"
#include "ban.h"

#include "databasetasks.h"

#include "creatureevent.h"

extern Game g_game;
extern ConfigManager g_config;
extern Chat* g_chat;
extern CreatureEvents* g_creatureEvents;

ProtocolCaster::LiveCastsMap ProtocolCaster::m_liveCasts;

ProtocolCaster::ProtocolCaster(Connection_ptr connection):
	ProtocolGame(connection),
	m_isLiveCaster(false)
{
}

void ProtocolCaster::releaseProtocol()
{
	stopLiveCast();

	ProtocolGame::releaseProtocol();
}

void ProtocolCaster::disconnectClient(const std::string& message)
{
	stopLiveCast();

	ProtocolGame::disconnectClient(message);
}

void ProtocolCaster::logout(bool displayEffect, bool forced)
{
	//dispatcher thread
	if (!player) {
		return;
	}

	if (!player->isRemoved()) {
		if (!forced) {
			if (!player->isAccessPlayer()) {
				if (player->getTile()->hasFlag(TILESTATE_NOLOGOUT)) {
					player->sendCancelMessage(RETURNVALUE_YOUCANNOTLOGOUTHERE);
					return;
				}

				if (!player->getTile()->hasFlag(TILESTATE_PROTECTIONZONE) && player->hasCondition(CONDITION_INFIGHT)) {
					player->sendCancelMessage(RETURNVALUE_YOUMAYNOTLOGOUTDURINGAFIGHT);
					return;
				}
			}

			//scripting event - onLogout
			if (!g_creatureEvents->playerLogout(player)) {
				//Let the script handle the error message
				return;
			}
		}

		if (displayEffect && player->getHealth() > 0) {
			g_game.addMagicEffect(player->getPosition(), CONST_ME_POFF);
		}
	}

	stopLiveCast();

	if (Connection_ptr connection = getConnection()) {
		connection->close();
	}

	g_game.removeCreature(player);
}

void ProtocolCaster::parsePacket(NetworkMessage& msg)
{
	if (!m_acceptPackets || g_game.getGameState() == GAME_STATE_SHUTDOWN || msg.getLength() <= 0) {
		return;
	}

	if (player && (player->isRemoved() || player->getHealth() <= 0)) {
		stopLiveCast();
	}

	ProtocolGame::parsePacket(msg);
}

bool ProtocolCaster::checkCommand(std::string text)
{
	if (text[0] == '/') {

		StringVec t = explodeString(text.substr(1, text.length()), " ", 1);
		if (t.size() > 0) {
			toLowerCaseString(t[0]);

			std::string command = t[0];

			if ((command == "mute") || (command == "unmute")) {
				if (t.size() == 2) {
					toLowerCaseString(t[1]);
					std::string toMute = t[1];

					if (toMute == "") {
						sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
						return true;
					}

					ProtocolSpectator* spectator = static_cast<ProtocolSpectator*>(getSpectatorByName(toMute));
					if (spectator) {
						if (command == "mute") {
							sendChannelMessage("", spectator->getSpectatorName() + " has been muted.", SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);

							muteList.push_back(spectator->getSpectatorId());
						} else {
							sendChannelMessage("", spectator->getSpectatorName() + " has been unmuted.", SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);

							auto it = std::find(muteList.begin(), muteList.end(), spectator->getSpectatorId());
							if (it != muteList.end()) {
								muteList.erase(it);
							}
						}
					}
					else {
						sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Spectator not found."), false);
					}
				} else {
					sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
				}
			} else if (command == "ban" || command == "unban") {
				if (t.size() == 2) {

					std::string toBan = t[1];
					toLowerCaseString(toBan);

					if (toBan == "") {
						sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
						return true;
					}

					bool ban = true;

					if (command == "unban") {
						ban = false;
					}

					if (ban) {
						ProtocolSpectator* spectator = static_cast<ProtocolSpectator*>(getSpectatorByName(toBan));
						if (spectator) {
							std::string name = spectator->getSpectatorName();

							sendChannelMessage("", name + " has been banned.", SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);
							toLowerCaseString(name);

							banMap.insert(std::make_pair(spectator->getIP(), name));

							removeSpectator(spectator);
							spectator->disconnect();
						} else {
							sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Spectator not found."), false);
						}
					} else {
						bool found = false;
						for (auto it : banMap) {
							if (toBan == it.second) {
								banMap.erase(it.first);
								found = true;
								break;
							}
						}

						if (found) {
							sendChannelMessage("",t[1] + " has been unbanned.", SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);
						} else {
							sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Spectator not found."), false);
						}
					}
				} else {
					sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
				}
			} else if (command == "spectators") {
				std::stringstream ss;
				if (getSpectatorCount() > 0) {
					ss << "Spectators:" << '\n';
					for (auto it : m_spectators) {
						ss << static_cast<ProtocolSpectator*>(it)->getSpectatorName() << '\n';
					}
				} else {
					ss << "No spectators." << '\n';
				}

				sendChannelMessage("", ss.str().c_str(), SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);
			} else if (command == "password") {
				if (t.size() == 2) {

					std::string newPassword = t[1];

					if (newPassword == "") {
						sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
						return true;
					}

					m_liveCastPassword = newPassword;

					sendChannelMessage("", "Casting new password: " + newPassword, SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);
				} else {
					sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
				}
			} else if (command == "kick") {
				if (t.size() == 2) {
					toLowerCaseString(t[1]);
					std::string toKick = t[1];

					if (toKick == "") {
						sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
						return true;
					}

					ProtocolSpectator* spectator = static_cast<ProtocolSpectator*>(getSpectatorByName(toKick));
					if (spectator) {
						sendChannelMessage("", spectator->getSpectatorName() + " has been kicked.", SpeakClasses::TALKTYPE_CHANNEL_O, CHANNEL_CAST, false);
						removeSpectator(spectator);
						spectator->disconnect();
					} else {
						sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Spectator not found."), false);
					}
				} else {
					sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Not enough parameters."), false);
				}
			} else {
				sendTextMessage(TextMessage(MESSAGE_STATUS_SMALL, "Invalid command."), false);
			}
		}

		return true;
	}

	return false;
}

void ProtocolCaster::parseSay(NetworkMessage& msg)
{
	std::string receiver;
	uint16_t channelId;

	SpeakClasses type = static_cast<SpeakClasses>(msg.getByte());
	switch (type) {
	case TALKTYPE_PRIVATE_TO:
	case TALKTYPE_PRIVATE_RED_TO:
		receiver = msg.getString();
		channelId = 0;
		break;

	case TALKTYPE_CHANNEL_Y:
	case TALKTYPE_CHANNEL_R1:
		channelId = msg.get<uint16_t>();
		break;

	default:
		channelId = 0;
		break;
	}

	const std::string text = msg.getString();
	if (text.length() > 255) {
		return;
	}

	if (channelId == CHANNEL_CAST) {
		if (checkCommand(text)) {
			return;
		}

		g_dispatcher.addTask(createTask(std::bind(&ProtocolGame::sendChannelMessage, this, player->getName(), text, TALKTYPE_CHANNEL_R1, channelId, true)));
	} else {
		addGameTask(&Game::playerSay, player->getID(), channelId, type, receiver, text);
	}
}

void ProtocolCaster::parseCloseChannel(NetworkMessage& msg)
{
	uint16_t channelId = msg.get<uint16_t>();
	if (channelId == CHANNEL_CAST) {
		stopLiveCast();
		sendTextMessage(TextMessage(MESSAGE_STATUS_DEFAULT, "Cast has been closed."), false);
	}
	else {
		addGameTask(&Game::playerCloseChannel, player->getID(), channelId);
	}
}

bool ProtocolCaster::startLiveCast(const std::string& password /*= ""*/)
{
	auto connection = getConnection();
	if (!g_config.getBoolean(ConfigManager::ENABLE_LIVE_CASTING) || m_isLiveCaster || !player || player->isRemoved() || !connection) {
		return false;
	}

	{
		//DO NOT do any send operations here
		if (m_liveCasts.size() >= getMaxLiveCastCount()) {
			return false;
		}

		m_spectatorsCount = 0;

		m_spectators.clear();
		muteList.clear();
		banMap.clear();

		m_liveCastName = player->getName();
		m_liveCastPassword = password;
		m_isLiveCaster = true;
		m_liveCasts.insert(std::make_pair(player, this));
	}

	registerLiveCast();
	//Send a "dummy" channel
	sendChannel(CHANNEL_CAST, LIVE_CAST_CHAT_NAME, nullptr, nullptr);
	return true;
}

bool ProtocolCaster::stopLiveCast()
{
	if (!m_isLiveCaster) {
		return false;
	}

	CastSpectatorVec spectators;

	std::swap(spectators, m_spectators);
	m_isLiveCaster = false;
	m_liveCasts.erase(player);

	for (auto& spectator : spectators) {
		spectator->setPlayer(nullptr);
		spectator->disconnect();
		spectator->unRef();
	}

	m_spectators.clear();
	muteList.clear();
	banMap.clear();

	if (player) {
		unregisterLiveCast();
	}

	return true;
}

void ProtocolCaster::clearLiveCastInfo()
{
	static std::once_flag flag;
	std::call_once(flag, []() {
		assert(g_game.getGameState() == GAME_STATE_INIT);
		std::ostringstream query;
		query << "DELETE FROM `live_casts`;";
		g_databaseTasks.addTask(query.str());
	});
}

void ProtocolCaster::registerLiveCast()
{
	std::ostringstream query;
	query << "INSERT into `live_casts` (`player_id`, `cast_name`, `password`) VALUES (" << player->getGUID() << ", '"
		<< getLiveCastName() << "', " << isPasswordProtected() << ");";
	g_databaseTasks.addTask(query.str());
}

void ProtocolCaster::unregisterLiveCast()
{
	std::ostringstream query;
	query << "DELETE FROM `live_casts` WHERE `player_id`=" << player->getGUID() << ";";
	g_databaseTasks.addTask(query.str());
}

void ProtocolCaster::updateLiveCastInfo()
{
	std::ostringstream query;
	query << "UPDATE `live_casts` SET `cast_name`='" << getLiveCastName() << "', `password`="
		<< isPasswordProtected() << ", `spectators`=" << getSpectatorCount()
		<< " WHERE `player_id`=" << player->getGUID() << ";";
	g_databaseTasks.addTask(query.str());
}

void ProtocolCaster::addSpectator(ProtocolGame* spectatorClient)
{
	//DO NOT do any send operations here
	m_spectatorsCount++;
	m_spectators.push_back(spectatorClient);
	spectatorClient->addRef();

	std::stringstream ss;
	ss << "Spectator(" << m_spectatorsCount << ")";

	static_cast<ProtocolSpectator*>(spectatorClient)->setSpectatorName(ss.str().c_str());
	static_cast<ProtocolSpectator*>(spectatorClient)->setSpectatorId(m_spectatorsCount);

	updateLiveCastInfo();
}

void ProtocolCaster::removeSpectator(ProtocolGame* spectatorClient)
{
	//DO NOT do any send operations here
	auto it = std::find(m_spectators.begin(), m_spectators.end(), spectatorClient);
	if (it != m_spectators.end()) {
		m_spectators.erase(it);
		spectatorClient->unRef();
	}
	updateLiveCastInfo();
}

ProtocolGame* ProtocolCaster::getSpectatorByName(std::string name)
{
	std::string tmpName = name;
	toLowerCaseString(tmpName);

	for (auto t : m_spectators) {
		std::string tmp = static_cast<ProtocolSpectator*>(t)->getSpectatorName();
		toLowerCaseString(tmp);
		if (tmp == tmpName) {
			return t;
		}
	}

	return nullptr;
}