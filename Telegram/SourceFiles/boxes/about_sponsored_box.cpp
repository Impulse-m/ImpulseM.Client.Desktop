/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "boxes/about_sponsored_box.h"

#include "core/impulse_build_links.h"
#include "lang/lang_keys.h"
#include "ui/layers/generic_box.h"
#include "ui/text/text_utilities.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "styles/style_boxes.h"
#include "styles/style_layers.h"

namespace Ui {

void AboutSponsoredBox(not_null<Ui::GenericBox*> box) {
	box->setTitle(tr::lng_sponsored_title());
	box->setWidth(st::boxWideWidth);
	box->addButton(tr::lng_box_ok(), [=] { box->closeBox(); });

	const auto &stLabel = st::aboutLabel;
	auto text1 = tr::lng_sponsored_info_description1_linked(
		lt_link,
		rpl::combine(
			tr::lng_sponsored_info_description1_link(),
			tr::lng_sponsored_info_description1_url()
		) | rpl::map([](const QString &text, const QString &path) {
			return Impulse::BuildLinks::SiteLinkText(text, path);
		}),
		tr::rich);
	box->addRow(object_ptr<FlatLabel>(box, std::move(text1), stLabel));

	const auto info2 = box->addRow(object_ptr<FlatLabel>(box, stLabel));
	info2->setText(tr::lng_sponsored_info_description2(tr::now));
}

} // namespace Ui
