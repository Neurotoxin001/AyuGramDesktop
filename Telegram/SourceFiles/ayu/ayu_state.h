// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

#include "data/data_types.h"

#include <optional>
#include <unordered_map>
#include <unordered_set>

class HistoryItem;

namespace Main {
class Session;
} // namespace Main

namespace AyuState {

using HiddenMessageIds = std::unordered_set<MsgId>;
using HiddenMessages = std::unordered_map<PeerId, HiddenMessageIds>;

void setHidden(
	not_null<Main::Session*> session,
	const MessageIdsList &ids,
	bool hidden);
void clearHidden(not_null<Main::Session*> session, PeerId peerId);
void clearAllHidden(not_null<Main::Session*> session);
const HiddenMessageIds *getHiddenMessages(
	not_null<Main::Session*> session,
	PeerId peerId);
const HiddenMessages &getAllHiddenMessages(
	not_null<Main::Session*> session);
bool hasAnyHiddenMessagesAll(not_null<Main::Session*> session);
bool hasHiddenFlag(not_null<HistoryItem*> item);
bool isHidden(not_null<HistoryItem*> item);

void showHiddenMessages(
	not_null<Main::Session*> session,
	PeerId peerId,
	bool show);
std::optional<bool> hiddenMessagesShown(
	not_null<Main::Session*> session,
	PeerId peerId);

void showAllHiddenMessages(not_null<Main::Session*> session, bool show);
std::optional<bool> allHiddenMessagesShown(
	not_null<Main::Session*> session);

void load();
void migrateLegacy();

void setDisableGhostModeOnStoryClose(Main::Session *session);
void disableGhostModeOnStoryClose(Main::Session *session);

}
