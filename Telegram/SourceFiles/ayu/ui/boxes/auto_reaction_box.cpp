// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ui/boxes/auto_reaction_box.h"

#include "ayu/ayu_settings.h"
#include "data/data_message_reactions.h"
#include "data/data_session.h"
#include "lang_auto.h"
#include "main/main_session.h"
#include "ui/layers/generic_box.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/emoji_config.h"

#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace AyuUi {
namespace {

[[nodiscard]] QString ResolveReaction(
		not_null<Main::Session*> session,
		QString value) {
	value = value.trimmed();
	auto length = 0;
	const auto emoji = Ui::Emoji::Find(value, &length);
	if (!emoji || length != value.size()) {
		return {};
	}
	const auto &available = session->data().reactions().list(
		Data::Reactions::Type::All);
	for (const auto &reaction : available) {
		const auto candidate = reaction.id.emoji();
		if (candidate == value || Ui::Emoji::Find(candidate) == emoji) {
			return candidate;
		}
	}
	return {};
}

} // namespace

void FillAutoReactionBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session) {
	const auto current = AyuSettings::getInstance().autoReaction(session);
	session->data().reactions().refreshDefault();

	box->setTitle(tr::ayu_AutoReactionTitle());
	box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		tr::ayu_AutoReactionDescription(),
		st::boxLabel));
	const auto enabled = box->addRow(object_ptr<Ui::Checkbox>(
		box,
		tr::ayu_AutoReactionEnabled(tr::now),
		current.enabled,
		st::defaultBoxCheckbox));
	const auto chatId = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		tr::ayu_AutoReactionChatId(),
		current.chatId ? QString::number(current.chatId) : QString()));
	const auto userId = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		tr::ayu_AutoReactionUserId(),
		current.userId ? QString::number(current.userId) : QString()));
	const auto reaction = box->addRow(object_ptr<Ui::InputField>(
		box,
		st::defaultInputField,
		Ui::InputField::Mode::SingleLine,
		tr::ayu_AutoReactionEmoji(),
		current.reaction));
	const auto error = box->addRow(object_ptr<Ui::FlatLabel>(
		box,
		st::boxLabel));
	error->setTextColorOverride(st::boxTextFgError->c);
	const auto reactionsReady = box->lifetime().make_state<bool>(
		!session->data().reactions().list(
			Data::Reactions::Type::All).empty());
	session->data().reactions().defaultUpdates(
	) | rpl::on_next([=] {
		*reactionsReady = true;
	}, box->lifetime());

	enabled->checkedValue(
	) | rpl::on_next([=](bool checked) {
		chatId->setDisabled(!checked);
		userId->setDisabled(!checked);
		reaction->setDisabled(!checked);
	}, box->lifetime());

	const auto save = [=] {
		if (!enabled->checked()) {
			auto rule = current;
			rule.enabled = false;
			AyuSettings::getInstance().setAutoReaction(session, rule);
			box->closeBox();
			return;
		}

		auto chatIdOk = false;
		const auto parsedChatId = chatId->getLastText(
		).trimmed().toLongLong(&chatIdOk);
		if (!chatIdOk || !IsValidAutoReactionChatId(parsedChatId)) {
			error->setText(tr::ayu_AutoReactionInvalidChatId(tr::now));
			chatId->showError();
			return;
		}

		auto userIdOk = false;
		const auto parsedUserId = userId->getLastText(
		).trimmed().toULongLong(&userIdOk);
		if (!userIdOk || !IsValidAutoReactionUserId(parsedUserId)) {
			error->setText(tr::ayu_AutoReactionInvalidUserId(tr::now));
			userId->showError();
			return;
		}

		if (!*reactionsReady) {
			session->data().reactions().refreshDefault();
			error->setText(tr::ayu_AutoReactionLoading(tr::now));
			reaction->showError();
			return;
		}
		const auto parsedReaction = ResolveReaction(
			session,
			reaction->getLastText());
		if (parsedReaction.isEmpty()) {
			error->setText(tr::ayu_AutoReactionInvalidEmoji(tr::now));
			reaction->showError();
			return;
		}

		AyuSettings::getInstance().setAutoReaction(session, {
			.enabled = true,
			.chatId = parsedChatId,
			.userId = parsedUserId,
			.reaction = parsedReaction,
		});
		box->closeBox();
	};

	chatId->submits(
	) | rpl::on_next([=] {
		userId->setFocusFast();
	}, box->lifetime());
	userId->submits(
	) | rpl::on_next([=] {
		reaction->setFocusFast();
	}, box->lifetime());
	reaction->submits(
	) | rpl::on_next(save, box->lifetime());

	box->addButton(tr::lng_settings_save(), save);
	box->addButton(tr::lng_cancel(), [=] { box->closeBox(); });
}

} // namespace AyuUi
