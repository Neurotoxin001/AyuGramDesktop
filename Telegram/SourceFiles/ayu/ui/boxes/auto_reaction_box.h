// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#pragma once

namespace Main {
class Session;
} // namespace Main

namespace Ui {
class GenericBox;
} // namespace Ui

namespace AyuUi {

void FillAutoReactionBox(
	not_null<Ui::GenericBox*> box,
	not_null<Main::Session*> session);

} // namespace AyuUi
