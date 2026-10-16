/*
BGH LIVE Front
Copyright (C) 2026 Barrington Gospel Hall

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/
// The "BGH LIVE Front" panel in OBS: a QR code that opens and pairs the phone remote, the address and 3-digit
// code for phones whose camera won't read it, whether the OPTOMA projector is connected, and the paired phones,
// each with a Remove button. Tools → BGH LIVE Front brings the panel back if it was closed.

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <plugin-support.h>

#include <QComboBox>
#include <QDockWidget>
#include <QGuiApplication>
#include <QFrame>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPixmap>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "front.h"
#include "qrcodegen.hpp"

namespace front {

namespace {

QPixmap qrPixmap(const std::string &text, int scale)
{
	using qrcodegen::QrCode;
	const QrCode qr = QrCode::encodeText(text.c_str(), QrCode::Ecc::MEDIUM);
	const int border = 2, n = qr.getSize(), size = (n + border * 2) * scale;
	QImage img(size, size, QImage::Format_RGB32);
	img.fill(Qt::white);
	QPainter p(&img);
	p.setPen(Qt::NoPen);
	p.setBrush(Qt::black);
	for (int y = 0; y < n; y++)
		for (int x = 0; x < n; x++)
			if (qr.getModule(x, y))
				p.drawRect((x + border) * scale, (y + border) * scale, scale, scale);
	p.end();
	return QPixmap::fromImage(img);
}

QString ago(int64_t t)
{
	if (!t)
		return "never";
	const int64_t min = (nowMs() - t) / 60000;
	if (min < 2)
		return "just now";
	if (min < 60)
		return QString("%1 min ago").arg(min);
	if (min < 60 * 24)
		return QString("%1 h ago").arg(min / 60);
	return QString("%1 days ago").arg(min / (60 * 24));
}

class FrontDock : public QWidget {
public:
	FrontDock()
	{
		auto *outer = new QVBoxLayout(this);
		outer->setContentsMargins(0, 0, 0, 0);
		auto *scroll = new QScrollArea(this);
		scroll->setWidgetResizable(true);
		scroll->setFrameShape(QFrame::NoFrame);
		outer->addWidget(scroll);
		auto *body = new QWidget();
		scroll->setWidget(body);
		lay = new QVBoxLayout(body);
		lay->setContentsMargins(10, 10, 10, 10);
		lay->setSpacing(8);

		auto *pairRow = new QHBoxLayout();
		pairRow->setSpacing(12);
		qr = new QLabel();
		qr->setFixedSize(150, 150);
		qr->setScaledContents(true);
		qr->setStyleSheet("background:white;border-radius:6px;");
		pairRow->addWidget(qr, 0, Qt::AlignTop);
		auto *steps = new QVBoxLayout();
		steps->setSpacing(4);
		auto *hint = new QLabel("Scan with a phone on the church Wi-Fi, or open");
		hint->setWordWrap(true);
		steps->addWidget(hint);
		url = new QLabel();
		url->setTextInteractionFlags(Qt::TextSelectableByMouse);
		url->setStyleSheet("font-weight:bold;");
		url->setWordWrap(true);
		steps->addWidget(url);
		steps->addWidget(new QLabel("and type"));
		code = new QLabel();
		code->setStyleSheet("font-size:30px;font-weight:800;letter-spacing:6px;color:#F5B700;");
		steps->addWidget(code);
		steps->addStretch(1);
		pairRow->addLayout(steps, 1);
		lay->addLayout(pairRow);

		// Which screen is the projector. Remembered by the screen's name, so Windows numbering screens
		// differently after a restart or a replug doesn't move it.
		auto *pjHead = new QLabel("PROJECTOR SCREEN");
		pjHead->setStyleSheet("font-size:11px;font-weight:bold;letter-spacing:1px;color:#8e919a;margin-top:6px;");
		lay->addWidget(pjHead);
		screenBox = new QComboBox();
		screenBox->setToolTip("The screen the cast button on Controller phones opens OBS on");
		QObject::connect(screenBox, &QComboBox::activated, [this](int index) {
			setProjectorChoice(screenBox->itemData(index).toString().toStdString());
			last.clear();
			refresh();
			refreshState(); // phones see the new projector name straight away
		});
		lay->addWidget(screenBox);

		projector = new QLabel();
		projector->setWordWrap(true);
		projector->setTextInteractionFlags(Qt::TextSelectableByMouse);
		lay->addWidget(projector);

		// The same as the cast button on Controller phones: Program full screen on the projector, or close it
		projBtn = new QPushButton();
		projBtn->setMinimumHeight(40);
		projBtn->setCursor(Qt::PointingHandCursor);
		QObject::connect(projBtn, &QPushButton::clicked, [this]() {
			if (isProjecting()) {
				if (QMessageBox::question(this, "Stop projecting?",
							  "Are you sure you want to stop projecting?\n\nOBS will close on the projector and "
							  "the slideshow will show again.",
							  QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
					return;
				setProjecting(false);
			} else {
				const std::string err = setProjecting(true);
				if (!err.empty())
					QMessageBox::warning(this, "Can't project", QString::fromStdString(err));
			}
			last.clear();
			refreshState(); // phones' cast buttons follow straight away
			refresh();
		});
		lay->addWidget(projBtn);

		auto *head = new QLabel("PAIRED PHONES");
		head->setStyleSheet("font-size:11px;font-weight:bold;letter-spacing:1px;color:#8e919a;margin-top:6px;");
		lay->addWidget(head);
		list = new QVBoxLayout();
		list->setSpacing(4);
		lay->addLayout(list);
		lay->addStretch(1);
	}

	void refresh()
	{
		const auto addrs = localAddresses();
		const std::string address =
			addrs.empty() ? "" : "http://" + addrs.front() + ":" + std::to_string(kPort);
		const std::string c = pairCode(), k = pairKey();
		QScreen *pj = projectorScreen();
		const std::string choice = projectorChoice();
		std::string names;
		for (QScreen *s : QGuiApplication::screens())
			names += s->name().toStdString() + ";";
		std::string sig = address + "|" + c + "|" + k + "|" + (pj ? pj->name().toStdString() : "-") + "|" + choice +
				  "|" + names + "|" + (isProjecting() ? "P" : "-");
		const auto ph = phones();
		for (auto &p : ph)
			sig += "|" + p.id + p.name + p.mode + (p.online ? "1" : "0") + ago(p.lastSeen).toStdString();
		if (sig == last)
			return;
		last = sig;

		if (address.empty()) {
			qr->clear();
			url->setText("This laptop isn't on a network. Connect it to the church Wi-Fi.");
		} else {
			qr->setPixmap(qrPixmap(address + "/?k=" + k, 6));
			url->setText(QString::fromStdString(address));
		}
		code->setText(QString::fromStdString(c));

		// The list: any screen named OPTOMA (the default), then each connected screen by name
		screenBox->blockSignals(true);
		screenBox->clear();
		screenBox->addItem(QString("Any screen named %1").arg(kProjectorName), QString());
		bool chosenHere = choice.empty();
		int n = 1;
		for (QScreen *s : QGuiApplication::screens()) {
			const QSize size = s->size() * s->devicePixelRatio();
			screenBox->addItem(QString("%1 · Screen %2 · %3×%4").arg(s->name()).arg(n++).arg(size.width()).arg(size.height()),
					   s->name());
			if (s->name().toStdString() == choice)
				chosenHere = true;
		}
		if (!chosenHere)
			screenBox->addItem(QString("%1 (not connected)").arg(QString::fromStdString(choice)), QString::fromStdString(choice));
		const int at = screenBox->findData(QString::fromStdString(choice));
		screenBox->setCurrentIndex(at < 0 ? 0 : at);
		screenBox->blockSignals(false);

		if (pj) {
			projector->setText(QString("<b>%1</b> is connected. The cast button opens OBS on it.")
						   .arg(pj->name().toHtmlEscaped()));
			projector->setStyleSheet("color:#3ccf7e;");
		} else if (choice.empty()) {
			projector->setText(QString("No screen with <b>%1</b> in its name is connected, so the cast button won't "
						   "work. Check the projector's HDMI cable, or choose its screen above.")
						   .arg(kProjectorName));
			projector->setStyleSheet("color:#ff8a3d;");
		} else {
			projector->setText(QString("<b>%1</b> isn't connected, so the cast button won't work. Check the "
						   "projector's HDMI cable, or choose another screen above.")
						   .arg(QString::fromStdString(choice).toHtmlEscaped()));
			projector->setStyleSheet("color:#ff8a3d;");
		}

		const bool on = isProjecting();
		projBtn->setEnabled(on || pj != nullptr);
		projBtn->setText(on ? QString("Projecting on %1 · Stop").arg(QString::fromStdString(projectorLabel()))
				    : QString("Project on %1").arg(QString::fromStdString(projectorLabel())));
		projBtn->setStyleSheet(on ? "QPushButton{background:#F5B700;color:#132932;font-weight:bold;border-radius:6px;}"
					  : "QPushButton{font-weight:bold;}");

		while (QLayoutItem *it = list->takeAt(0)) {
			if (QWidget *w = it->widget())
				w->deleteLater();
			delete it;
		}
		if (ph.empty()) {
			auto *none = new QLabel("No phones yet.");
			none->setStyleSheet("color:#8e919a;");
			list->addWidget(none);
		}
		for (auto &p : ph) {
			auto *row = new QFrame();
			row->setStyleSheet("QFrame{background:rgba(255,255,255,0.05);border-radius:5px;}");
			auto *h = new QHBoxLayout(row);
			h->setContentsMargins(8, 5, 8, 5);
			auto *dot = new QLabel("●");
			dot->setStyleSheet(p.online ? "color:#3ccf7e;background:transparent;" : "color:#666;background:transparent;");
			h->addWidget(dot);
			auto *name = new QLabel(QString("<b>%1</b><br><span style='color:#8e919a;font-size:11px;'>%2</span>")
							.arg(QString::fromStdString(p.name).toHtmlEscaped())
							.arg(p.online ? "Connected" : "Last used " + ago(p.lastSeen)));
			name->setStyleSheet("background:transparent;");
			h->addWidget(name, 1);
			auto *mode = new QLabel(p.mode == "presenter"    ? "PRESENTER"
						: p.mode == "controller" ? "CONTROLLER"
									 : "CHOOSING");
			mode->setStyleSheet(p.mode == "presenter" ? "color:#F5B700;font-size:10px;font-weight:bold;background:transparent;"
								  : "color:#b8bbc4;font-size:10px;font-weight:bold;background:transparent;");
			h->addWidget(mode);
			auto *rm = new QPushButton("Remove");
			const std::string id = p.id;
			QObject::connect(rm, &QPushButton::clicked, [this, id]() {
				removePhone(id);
				last.clear();
				refresh();
			});
			h->addWidget(rm);
			list->addWidget(row);
		}
	}

private:
	QVBoxLayout *lay = nullptr, *list = nullptr;
	QLabel *qr = nullptr, *url = nullptr, *code = nullptr, *projector = nullptr;
	QComboBox *screenBox = nullptr;
	QPushButton *projBtn = nullptr;
	std::string last;
};

FrontDock *g_dock = nullptr;

void showDock(void *)
{
	if (!g_dock)
		return;
	for (QWidget *w = g_dock; w; w = w->parentWidget())
		if (auto *d = qobject_cast<QDockWidget *>(w)) {
			d->setVisible(true);
			d->raise();
			return;
		}
	g_dock->show();
}

} // namespace

void dockCreate()
{
	if (g_dock)
		return;
	g_dock = new FrontDock();
	if (!obs_frontend_add_dock_by_id("bgh-live-front", "BGH LIVE Front", g_dock))
		obs_log(LOG_WARNING, "couldn't add the BGH LIVE Front panel");
	obs_frontend_add_tools_menu_item("BGH LIVE Front", showDock, nullptr);
}

void dockRefresh()
{
	if (g_dock)
		g_dock->refresh();
}

} // namespace front
