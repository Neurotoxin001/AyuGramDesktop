/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "history/view/history_view_schedule_box.h"

#include "api/api_common.h"
#include "chat_helpers/compose/compose_show.h"
#include "core/application.h"
#include "core/core_settings.h"
#include "data/components/scheduled_messages.h"
#include "data/data_document.h"
#include "data/data_media_types.h"
#include "data/data_messages.h"
#include "data/data_peer.h"
#include "data/data_peer_values.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_keys.h"
#include "base/event_filter.h"
#include "base/qt/qt_key_modifiers.h"
#include "base/unixtime.h"
#include "lottie/lottie_icon.h"
#include "ui/controls/warning_tooltip.h"
#include "ui/effects/ripple_animation.h"
#include "ui/layers/generic_box.h"
#include "ui/dynamic_thumbnails.h"
#include "ui/rect.h"
#include "ui/text/text_utilities.h"
#include "ui/ui_utility.h"
#include "ui/widgets/fields/input_field.h"
#include "ui/widgets/fields/number_input.h"
#include "ui/widgets/checkbox.h"
#include "ui/widgets/labels.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/popup_menu.h"
#include "ui/wrap/padding_wrap.h"
#include "main/main_session.h"
#include "menu/menu_send.h"
#include "settings/sections/settings_premium.h"
#include "styles/style_boxes.h"
#include "styles/style_choose_date_time.h"
#include "styles/style_info.h"
#include "styles/style_menu_icons.h"
#include "styles/style_widgets.h"

#include <QtCore/QDateTime>
#include <QtGui/QWheelEvent>

#include <string>

namespace HistoryView::details {

not_null<Main::Session*> SessionFromShow(
		const std::shared_ptr<ChatHelpers::Show> &show) {
	return &show->session();
}

} // namespace HistoryView::details

namespace HistoryView {
namespace {

[[nodiscard]] std::shared_ptr<Ui::DynamicImage> ItemThumbnail(
		not_null<HistoryItem*> item) {
	const auto media = item->media();
	if (!media) {
		return nullptr;
	} else if (const auto photo = media->photo()) {
		return Ui::MakePhotoThumbnail(photo, item->fullId());
	} else if (const auto document = media->document()) {
		if (document->isVideoFile()) {
			return Ui::MakeDocumentThumbnail(document, item->fullId());
		}
	}
	return nullptr;
}

[[nodiscard]] Fn<void(QDate, Ui::CalendarImageSetter)> ScheduledImageForDate(
		not_null<History*> history,
		MsgId topicRootId) {
	struct State {
		base::flat_map<QDate, std::shared_ptr<Ui::DynamicImage>> images;
		Ui::CalendarImageSetter setter;
		bool subscribed = false;
		rpl::lifetime lifetime;
	};
	const auto state = std::make_shared<State>();
	const auto raw = state.get();
	const auto session = &history->session();

	const auto refresh = [=] {
		const auto topic = topicRootId
			? history->peer->forumTopicFor(topicRootId)
			: nullptr;
		auto previous = base::take(raw->images);
		if (!topicRootId || topic) {
			auto &scheduled = session->scheduledMessages();
			const auto list = topic
				? scheduled.list(topic)
				: scheduled.list(history);
			for (const auto &fullId : list.ids) {
				const auto item = session->data().message(fullId);
				if (!item
					|| item->date() == Api::kScheduledUntilOnlineTimestamp) {
					continue;
				}
				const auto date = base::unixtime::parse(item->date()).date();
				if (raw->images.contains(date)) {
					continue;
				}
				if (auto image = ItemThumbnail(item)) {
					raw->images.emplace(date, std::move(image));
				}
			}
		}
		if (const auto setter = raw->setter) {
			for (const auto &[date, image] : raw->images) {
				setter(date, image);
			}
			for (const auto &[date, image] : previous) {
				if (!raw->images.contains(date)) {
					setter(date, nullptr);
				}
			}
		}
	};

	return [=](QDate date, Ui::CalendarImageSetter setter) {
		state->setter = std::move(setter);
		if (!state->subscribed) {
			state->subscribed = true;
			rpl::single(rpl::empty) | rpl::then(
				session->scheduledMessages().updates(history)
			) | rpl::on_next(refresh, state->lifetime);
		} else {
			for (const auto &[known, image] : state->images) {
				state->setter(known, image);
			}
		}
	};
}

void FillSendUntilOnlineMenu(
		not_null<Ui::IconButton*> button,
		Fn<void()> callback,
		const ScheduleBoxStyleArgs &style) {
	const auto menu = std::make_shared<base::unique_qptr<Ui::PopupMenu>>();
	button->setClickedCallback([=] {
		*menu = base::make_unique_q<Ui::PopupMenu>(
			button,
			*style.popupMenuStyle);
		(*menu)->addAction(
			tr::lng_scheduled_send_until_online(tr::now),
			std::move(callback),
			&st::menuIconWhenOnline);
		(*menu)->popup(QCursor::pos());
		return true;
	});
}

constexpr auto kNotifyRestFrame = 40;
constexpr auto kNotifyLastFrame = 80;
constexpr auto kNotifyTooltipDuration = crl::time(5000);

class NotifyToggleButton final : public Ui::RippleButton {
public:
	NotifyToggleButton(
		QWidget *parent,
		not_null<Ui::GenericBox*> box,
		const style::IconButton &st,
		std::shared_ptr<bool> silent);

protected:
	void paintEvent(QPaintEvent *e) override;

	QImage prepareRippleMask() const override;
	QPoint prepareRippleStartPosition() const override;

private:
	void toggle();
	[[nodiscard]] QRect rippleArea() const;

	const style::IconButton &_st;
	const not_null<Ui::GenericBox*> _box;
	const std::shared_ptr<bool> _silent;
	const std::unique_ptr<Lottie::Icon> _icon;
	Ui::WarningTooltip _tooltip;

};

NotifyToggleButton::NotifyToggleButton(
	QWidget *parent,
	not_null<Ui::GenericBox*> box,
	const style::IconButton &st,
	std::shared_ptr<bool> silent)
: RippleButton(parent, st.ripple)
, _st(st)
, _box(box)
, _silent(std::move(silent))
, _icon(Lottie::MakeIcon({
	.path = u":/icons/notify_toggle.lottie"_q,
	.color = &st::boxTitleCloseFg,
	.sizeOverride = Size(st::scheduleNotifyIconSize),
	.frame = kNotifyRestFrame,
})) {
	resize(_st.width, _st.height);
	setPointerCursor(true);
	setAccessibleName(tr::lng_send_silent_message(tr::now));

	clicks(
	) | rpl::on_next([=] { toggle(); }, lifetime());
}

void NotifyToggleButton::toggle() {
	const auto silent = !*_silent;
	*_silent = silent;
	_icon->animate(
		[=] { update(); },
		silent ? kNotifyRestFrame : 0,
		silent ? kNotifyLastFrame : kNotifyRestFrame);
	update();

	const auto outer = _box->getDelegate()->outerContainer();
	if (!outer) {
		return;
	}
	const auto parent = static_cast<Ui::RpWidget*>(outer.data());
	const auto button = this;
	_tooltip.show({
		.parent = parent,
		.target = button,
		.text = rpl::single(TextWithEntities{ silent
			? tr::lng_schedule_notify_off(tr::now)
			: tr::lng_schedule_notify_on(tr::now) }),
		.side = RectPart::Top | RectPart::Left,
		.countPosition = [=](QSize size) {
			const auto area = Ui::MapFrom(parent, button, button->rect());
			const auto &tip = st::defaultImportantTooltip;
			const auto middle = rect::center(area).x();
			auto left = middle + tip.arrowSkip - size.width();
			left = std::min(
				left,
				parent->width() - tip.margin.right() - size.width());
			left = std::max(left, tip.margin.left());
			left = std::max(
				left,
				middle + tip.arrow + tip.arrowSkipMin - size.width());
			left = std::min(left, middle - tip.arrow - tip.arrowSkipMin);
			return QPoint(
				left,
				area.y() - size.height() + st::scheduleNotifyTooltipShift);
		},
		.duration = kNotifyTooltipDuration,
	});
}

QRect NotifyToggleButton::rippleArea() const {
	const auto size = Size(_st.rippleAreaSize);
	return QRect(
		QPoint(
			(_st.width - size.width()) / 2,
			(_st.height - size.height()) / 2),
		size);
}

void NotifyToggleButton::paintEvent(QPaintEvent *e) {
	auto p = QPainter(this);
	const auto area = rippleArea();
	paintRipple(p, area.topLeft());
	_icon->paintInCenter(
		p,
		area,
		(isOver()
			? st::boxTitleCloseFgOver
			: st::boxTitleCloseFg)->c);
}

QImage NotifyToggleButton::prepareRippleMask() const {
	return Ui::RippleAnimation::EllipseMask(Size(_st.rippleAreaSize));
}

QPoint NotifyToggleButton::prepareRippleStartPosition() const {
	const auto result = mapFromGlobal(QCursor::pos())
		- rippleArea().topLeft();
	const auto area = Rect(Size(_st.rippleAreaSize));
	return area.contains(result)
		? result
		: DisabledRippleStartPosition();
}

} // namespace

ScheduleBoxStyleArgs::ScheduleBoxStyleArgs()
: topButtonStyle(&st::infoTopBarMenu)
, popupMenuStyle(&st::popupMenuWithIcons)
, chooseDateTimeArgs({}) {
}

TimeId DefaultScheduleTime() {
	return base::unixtime::now() + 600;
}

bool CanScheduleUntilOnline(not_null<PeerData*> peer) {
	if (const auto user = peer->asUser()) {
		return !user->isSelf()
			&& !user->isBot()
			&& !user->lastseen().isHidden()
			&& !user->starsPerMessageChecked()
			&& !user->isNotificationsUser();
	}
	return false;
}

void ScheduleBox(
		not_null<Ui::GenericBox*> box,
		not_null<Main::Session*> session,
		std::shared_ptr<ChatHelpers::Show> maybeShow,
		const Api::SendOptions &initialOptions,
		const SendMenu::Details &details,
		Fn<void(Api::SendOptions)> done,
		TimeId time,
		ScheduleBoxStyleArgs style) {
	struct State {
		Ui::NumberInput *interval = nullptr;
		Ui::Checkbox *useInterval = nullptr;
	};
	const auto state = box->lifetime().make_state<State>();
	const auto intervalCount = std::max(
		details.ungroupedFilesCount,
		details.forwardedPostsCount
			? details.forwardedPostsCount
			: details.forwardedMessagesCount);
	const auto intervalKey = details.barePeerId
		? "scheduled_media_interval/" + std::to_string(session->uniqueId())
			+ "/" + std::to_string(details.barePeerId)
		: std::string();
	constexpr auto kMaxIntervalMinutes = 366 * 24 * 60;
	constexpr auto kIntervalWheelStep = 10;
	const auto initialMinutes = std::clamp(
		intervalKey.empty()
			? initialOptions.scheduledMediaInterval / 60
			: Core::App().settings().readPref<int>(
				intervalKey,
				initialOptions.scheduledMediaInterval / 60),
		1,
		kMaxIntervalMinutes);
	const auto enabledKey = intervalKey + "/enabled";
	const auto initiallyEnabled = intervalKey.empty()
		? !details.forwardedMessagesCount
		: Core::App().settings().readPref<bool>(
			enabledKey,
			!details.forwardedMessagesCount);
	const auto repeat = std::make_shared<TimeId>(
		initialOptions.scheduleRepeatPeriod);
	const auto silent = std::make_shared<bool>(false);
	const auto submit = [=](Api::SendOptions options) {
		if (!options.scheduled) {
			return;
		}
		options.staggerForwardedMessages = false;
		if (state->interval && !state->useInterval->checked()) {
			options.scheduledMediaInterval = 0;
		}
		if (state->interval
			&& state->useInterval->checked()
			&& options.scheduled != Api::kScheduledUntilOnlineTimestamp) {
			const auto minutes = state->interval->getLastText().toInt();
			if (minutes <= 0) {
				state->interval->showError();
				state->interval->setFocusFast();
				return;
			}
			const auto lastScheduled = int64(options.scheduled)
				+ int64(minutes) * 60 * (intervalCount - 1);
			const auto maxScheduled = base::unixtime::serialize(
				QDateTime::currentDateTime().addYears(1)) - 1;
			if (lastScheduled > maxScheduled) {
				state->interval->showError();
				state->interval->setFocusFast();
				box->uiShow()->showToast(
					tr::lng_schedule_media_interval_too_long(tr::now));
				return;
			}
			options.scheduledMediaInterval = minutes * 60;
			options.staggerForwardedMessages = details.forwardedMessagesCount > 1;
			if (!intervalKey.empty()) {
				Core::App().settings().writePref<int>(intervalKey, minutes);
			}
		}
		if (state->useInterval
			&& !intervalKey.empty()
			&& options.scheduled != Api::kScheduledUntilOnlineTimestamp) {
			Core::App().settings().writePref<bool>(
				enabledKey,
				state->useInterval->checked());
		}
		// Pro tip: Hold Ctrl key to send a silent scheduled message!
		if (base::IsCtrlPressed() || *silent) {
			options.silent = true;
		}
		if (repeat) {
			options.scheduleRepeatPeriod = *repeat;
		}
		const auto copy = done;
		box->closeBox();
		copy(options);
	};
	const auto with = [=](TimeId scheduled) {
		auto result = initialOptions;
		result.scheduled = scheduled;
		return result;
	};
	const auto history = details.barePeerId
		? session->data().historyLoaded(PeerId(details.barePeerId))
		: nullptr;
	auto descriptor = Ui::ChooseDateTimeBox(box, {
		.title = (details.type == SendMenu::Type::Reminder
			? tr::lng_remind_title()
			: tr::lng_schedule_title()),
		.submit = tr::lng_schedule_button(),
		.done = [=](TimeId result) { submit(with(result)); },
		.time = time,
		.style = style.chooseDateTimeArgs,
		.dynamicImageForDate = (history
			? ScheduledImageForDate(history, MsgId(details.bareTopicRootId))
			: nullptr),
	});

	if (details.ungroupedFilesCount > 1 || details.forwardedMessagesCount > 1) {
		state->useInterval = box->addRow(
			object_ptr<Ui::Checkbox>(
				box,
				tr::lng_schedule_use_interval(tr::now),
				initiallyEnabled),
			style::al_top);
		const auto row = box->addRow(
			object_ptr<Ui::FixedHeightWidget>(
				box,
				st::scheduleMediaIntervalHeight),
			st::scheduleMediaIntervalMargin,
			style::al_top);
		const auto label = Ui::CreateChild<Ui::FlatLabel>(
			row,
			tr::lng_schedule_media_interval(),
			*style.chooseDateTimeArgs.labelStyle);
		const auto field = state->interval = Ui::CreateChild<Ui::NumberInput>(
			row,
			*style.chooseDateTimeArgs.dateFieldStyle,
			rpl::single(QString()),
			QString::number(initialMinutes),
			kMaxIntervalMinutes);
		field->setAccessibleName(tr::lng_schedule_media_interval(tr::now));
		state->useInterval->checkedValue() | rpl::on_next([=](bool enabled) {
			field->setEnabled(enabled);
			field->hideError();
		}, field->lifetime());
		base::install_event_filter(field, [=](not_null<QEvent*> event) {
			if (event->type() != QEvent::Wheel || !field->isEnabled()) {
				return base::EventFilterResult::Continue;
			}
			const auto direction = Ui::WheelDirection(
				static_cast<QWheelEvent*>(event.get()));
			if (direction) {
				const auto minutes = field->getLastText().toInt();
				field->setText(QString::number(std::clamp(
					minutes + direction * kIntervalWheelStep,
					1,
					kMaxIntervalMinutes)));
				field->hideError();
			}
			return base::EventFilterResult::Cancel;
		});
		row->widthValue() | rpl::on_next([=](int width) {
			field->resizeToWidth(st::scheduleTimeWidth);
			label->resizeToWidth(width
				- st::scheduleTimeWidth
				- st::scheduleMediaIntervalSkip);
			label->moveToLeft(0, (row->height() - label->height()) / 2);
			field->moveToRight(0, (row->height() - field->height()) / 2);
		}, row->lifetime());
		rpl::duplicate(descriptor.width) | rpl::on_next([=](int width) {
			state->useInterval->setNaturalWidth(width);
			row->setNaturalWidth(width);
		}, row->lifetime());
		QObject::connect(field, &Ui::NumberInput::submitted, box, [=] {
			submit(with(descriptor.collect()));
		});
	}

	if (repeat) {
		const auto boxShow = box->uiShow();
		const auto showPremiumPromo = [=] {
			if (session->premium()) {
				return false;
			}
			Settings::ShowPremiumPromoToast(
				Main::MakeSessionShow(boxShow, session),
				ChatHelpers::ResolveWindowDefault(),
				tr::lng_schedule_repeat_promo(
					tr::now,
					lt_link,
					tr::link(
						tr::bold(
							tr::lng_schedule_repeat_promo_link(tr::now))),
					tr::rich),
				u"schedule_repeat"_q);
			return true;
		};
		auto locked = Data::AmPremiumValue(
			session
		) | rpl::map([=](bool premium) {
			return !premium;
		});
		const auto row = box->addRow(Ui::ChooseRepeatPeriod(box, {
			.value = session->premium() ? *repeat : TimeId(),
			.locked = std::move(locked),
			.filter = showPremiumPromo,
			.changed = [=](TimeId value) { *repeat = value; },
			.test = session->isTestMode(),
		}), st::scheduleRepeatMargin, style::al_top);
		std::move(descriptor.width) | rpl::on_next([=](int width) {
			row->setNaturalWidth(width);
		}, row->lifetime());
	}

	using namespace SendMenu;
	const auto childType = (details.type == Type::Disabled)
		? Type::Disabled
		: Type::SilentOnly;
	const auto childDetails = Details{
		.type = childType,
		.effectAllowed = details.effectAllowed,
	};
	const auto sendAction = crl::guard(box, [=](Action action, Details) {
		Expects(action.type == ActionType::Send);

		auto options = with(descriptor.collect());
		if (action.options.silent) {
			options.silent = action.options.silent;
		}
		if (action.options.effectId) {
			options.effectId = action.options.effectId;
		}
		submit(options);
	});
	SetupMenuAndShortcuts(
		descriptor.submit.data(),
		maybeShow,
		[=] { return childDetails; },
		sendAction);

	if (details.type == Type::ScheduledToUser) {
		const auto sendUntilOnline = box->addTopButton(*style.topButtonStyle);
		const auto timestamp = Api::kScheduledUntilOnlineTimestamp;
		FillSendUntilOnlineMenu(
			sendUntilOnline.data(),
			[=] { submit(with(timestamp)); },
			style);
	}

	box->addTopButton(object_ptr<NotifyToggleButton>(
		box,
		box,
		*style.topButtonStyle,
		silent));
}

} // namespace HistoryView
