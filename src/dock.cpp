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

#include <QButtonGroup>
#include <QCheckBox>
#include <QFormLayout>
#include <QLineEdit>
#include <QSlider>
#include <QSpinBox>
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
		// Never shrinks to a sliver: room for the QR code and the settings, whatever OBS's layout does
		setMinimumSize(280, 360);
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

		// When OBS is projecting and a slideshow is also on the projector, which one the audience sees
		auto *topHead = new QLabel("IN FRONT ON THE PROJECTOR");
		topHead->setStyleSheet("font-size:11px;font-weight:bold;letter-spacing:1px;color:#8e919a;margin-top:6px;");
		lay->addWidget(topHead);
		auto *topRow = new QHBoxLayout();
		topRow->setSpacing(0);
		topAuto = new QPushButton("Automatic");
		topObs = new QPushButton("OBS Projector");
		topPres = new QPushButton("Presentation");
		auto *group = new QButtonGroup(this);
		group->setExclusive(true);
		for (QPushButton *b : {topAuto, topObs, topPres}) {
			b->setCheckable(true);
			b->setMinimumHeight(34);
			b->setCursor(Qt::PointingHandCursor);
			group->addButton(b);
			topRow->addWidget(b, 1);
		}
		topAuto->setStyleSheet(segStyle("6px 0 0 6px"));
		topObs->setStyleSheet(segStyle("0"));
		topPres->setStyleSheet(segStyle("0 6px 6px 0"));
		topAuto->setToolTip("PowerPoint and full-screen presentations on top, OBS's projector second, everything else underneath");
		QObject::connect(topAuto, &QPushButton::clicked, [this]() { chooseTop("auto"); });
		topObs->setToolTip("OBS's projector covers the slideshow while it's open");
		topPres->setToolTip("The slideshow stays in front; OBS's projector waits behind it");
		QObject::connect(topObs, &QPushButton::clicked, [this]() { chooseTop("obs"); });
		QObject::connect(topPres, &QPushButton::clicked, [this]() { chooseTop("presentation"); });
		lay->addLayout(topRow);
		topNote = new QLabel();
		topNote->setWordWrap(true);
		topNote->setStyleSheet("color:#8e919a;font-size:11px;");
		lay->addWidget(topNote);

		// The Baptism Cam: an OBSBOT Tiny 2, moved through OBSBOT Center
		auto *camHead = new QLabel("CAMERA");
		camHead->setStyleSheet("font-size:11px;font-weight:bold;letter-spacing:1px;color:#8e919a;margin-top:6px;");
		lay->addWidget(camHead);
		camStatus = new QLabel();
		camStatus->setWordWrap(true);
		lay->addWidget(camStatus);
		camWarn = new QLabel();
		camWarn->setWordWrap(true);
		camWarn->setStyleSheet("color:#ff8a3d;");
		lay->addWidget(camWarn);
		camStartBtn = new QPushButton("Open OBSBOT Center");
		QObject::connect(camStartBtn, &QPushButton::clicked, [this]() {
			const std::string err = camStartCenterNow();
			if (!err.empty())
				QMessageBox::warning(this, "OBSBOT Center", QString::fromStdString(err));
			last.clear();
			refresh();
		});
		lay->addWidget(camStartBtn);
		const CamPanel cp = camPanel();
		camAuto = new QCheckBox("Start OBSBOT Center when OBS opens");
		camAwake = new QCheckBox("Keep the laptop awake while OBS is open");
		camUpside = new QCheckBox("Camera is mounted upside down");
		camAuto->setChecked(cp.autoCenter);
		camAwake->setChecked(cp.keepAwake);
		camUpside->setChecked(cp.upside);
		for (QCheckBox *b : {camAuto, camAwake, camUpside}) {
			QObject::connect(b, &QCheckBox::toggled, [this]() { saveCam(); });
			lay->addWidget(b);
		}
		auto *speedRow = new QHBoxLayout();
		speedRow->addWidget(new QLabel("Arrows top speed"));
		camSpeed = new QSlider(Qt::Horizontal);
		camSpeed->setRange(30, 100);
		camSpeed->setSingleStep(5);
		camSpeed->setPageStep(10);
		camSpeed->setValue(cp.topSpeed);
		camSpeedV = new QLabel(QString("%1%").arg(cp.topSpeed));
		camSpeedV->setMinimumWidth(40);
		QObject::connect(camSpeed, &QSlider::valueChanged, [this](int v) { camSpeedV->setText(QString("%1%").arg(v)); });
		QObject::connect(camSpeed, &QSlider::sliderReleased, [this]() { saveCam(); });
		speedRow->addWidget(camSpeed, 1);
		speedRow->addWidget(camSpeedV);
		lay->addLayout(speedRow);
		camCheckBtn = new QPushButton("Check directions");
		camCheckBtn->setToolTip("Nudges the camera left, then back. If the picture moves right first, tick \"mounted upside down\".");
		QObject::connect(camCheckBtn, &QPushButton::clicked, [this]() {
			const std::string err = camCheckDirections();
			if (!err.empty())
				QMessageBox::warning(this, "Check directions", QString::fromStdString(err));
			else
				camWarn->setText("Checking: the picture should move left, then come back. If it went right first, tick "
						 "\"Camera is mounted upside down\" and check again.");
		});
		lay->addWidget(camCheckBtn);
		auto *form = new QFormLayout();
		form->setContentsMargins(0, 0, 0, 0);
		camDir = new QLineEdit(QString::fromStdString(cp.centerDir));
		camPort = new QSpinBox();
		camReply = new QSpinBox();
		for (QSpinBox *b : {camPort, camReply})
			b->setRange(1024, 65535);
		camPort->setValue(cp.oscPort);
		camReply->setValue(cp.replyPort);
		camPort->setToolTip("The OSC port set in OBSBOT Center");
		camReply->setToolTip("Where OBSBOT Center sends its answers");
		QObject::connect(camDir, &QLineEdit::editingFinished, [this]() { saveCam(); });
		QObject::connect(camPort, &QSpinBox::editingFinished, [this]() { saveCam(); });
		QObject::connect(camReply, &QSpinBox::editingFinished, [this]() { saveCam(); });
		form->addRow("OBSBOT Center folder", camDir);
		form->addRow("OSC port", camPort);
		form->addRow("Reply port", camReply);
		lay->addLayout(form);
		// The hymn book's Presenter Mode, opened from this laptop. The QR code also pairs a new phone.
		auto *hyHead = new QLabel("HYMN VERSES");
		hyHead->setStyleSheet("font-size:11px;font-weight:bold;letter-spacing:1px;color:#8e919a;margin-top:6px;");
		lay->addWidget(hyHead);
		auto *hyRow = new QHBoxLayout();
		hyRow->setSpacing(12);
		hymnQr = new QLabel();
		hymnQr->setFixedSize(110, 110);
		hymnQr->setScaledContents(true);
		hymnQr->setStyleSheet("background:white;border-radius:6px;");
		hyRow->addWidget(hymnQr, 0, Qt::AlignTop);
		auto *hySteps = new QVBoxLayout();
		hySteps->setSpacing(4);
		auto *hyHint = new QLabel("Scan to open the hymn book on a phone and show verses on the projector. Or open");
		hyHint->setWordWrap(true);
		hySteps->addWidget(hyHint);
		hymnUrl = new QLabel();
		hymnUrl->setTextInteractionFlags(Qt::TextSelectableByMouse);
		hymnUrl->setStyleSheet("font-weight:bold;");
		hymnUrl->setWordWrap(true);
		hySteps->addWidget(hymnUrl);
		hySteps->addStretch(1);
		hyRow->addLayout(hySteps, 1);
		lay->addLayout(hyRow);
		hymnScene = new QLabel();
		hymnScene->setWordWrap(true);
		hymnScene->setTextInteractionFlags(Qt::TextSelectableByMouse);
		lay->addWidget(hymnScene);

		auto *head = new QLabel("PAIRED PHONES");
		head->setStyleSheet("font-size:11px;font-weight:bold;letter-spacing:1px;color:#8e919a;margin-top:6px;");
		lay->addWidget(head);
		list = new QVBoxLayout();
		list->setSpacing(4);
		lay->addLayout(list);
		lay->addStretch(1);
	}

	QSize sizeHint() const override { return QSize(320, 640); }

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
				  "|" + names + "|" + (isProjecting() ? "P" : "-") + "|" + onTop() + "|" +
				  hymnSceneProblem("nbh") + "|" + hymnSceneProblem("soterio");
		{
			const CamPanel c2 = camPanel();
			sig += "|cam" + c2.err + (c2.usb ? "u" : "-") + (c2.running ? "r" : "-") + (c2.answering ? "a" : "-") +
			       (c2.starting ? "s" : "-") + (c2.busy ? "b" : "-") + (c2.hasSource ? "h" : "-") +
			       (c2.crashed ? "c" : "-");
		}
		const auto ph = phones();
		for (auto &p : ph)
			sig += "|" + p.id + p.name + p.mode + (p.online ? "1" : "0") + ago(p.lastSeen).toStdString();
		if (sig == last)
			return;
		last = sig;

		if (address.empty()) {
			qr->clear();
			hymnQr->clear();
			url->setText("This laptop isn't on a network. Connect it to the church Wi-Fi.");
			hymnUrl->setText("");
		} else {
			qr->setPixmap(qrPixmap(address + "/?k=" + k, 6));
			url->setText(QString::fromStdString(address));
			// Pairs the phone first if it needs it, then opens the hymn book
			hymnQr->setPixmap(qrPixmap(address + "/?k=" + k + "&next=hymns", 5));
			hymnUrl->setText(QString::fromStdString(address + "/hymns"));
		}
		// Each hymn book's scene: there, or how to add it
		{
			QString lines;
			bool all = true;
			for (const char *book : {"nbh", "soterio"}) {
				const std::string problem = hymnSceneProblem(book);
				all = all && problem.empty();
				lines += QString("%1<b>%2</b>: %3")
						 .arg(lines.isEmpty() ? "" : "<br>")
						 .arg(QString(front::hymnScene(book)).toHtmlEscaped())
						 .arg(problem.empty() ? QString("ready") : QString::fromStdString(problem).toHtmlEscaped());
			}
			hymnScene->setText(lines + "<br>Choosing a hymn book on the remote switches OBS to its scene and projects it.");
			hymnScene->setStyleSheet(all ? "color:#3ccf7e;" : "color:#ff8a3d;");
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
		const std::string mode = onTop();
		const bool presFront = mode == "presentation";
		projBtn->setEnabled(on || pj != nullptr);
		projBtn->setText(on ? QString("Projecting on %1%2 · Stop")
					      .arg(QString::fromStdString(projectorLabel()))
					      .arg(presFront ? " (behind the presentation)" : "")
				    : QString("Project on %1").arg(QString::fromStdString(projectorLabel())));
		topAuto->setChecked(mode == "auto");
		topObs->setChecked(mode == "obs");
		topPres->setChecked(presFront);
		topNote->setText(mode == "auto"
					 ? "PowerPoint and full-screen presentations stay on top. OBS's projector comes second and covers "
					   "everything else, such as an ordinary browser window."
				 : presFront ? "The slideshow stays in front. OBS's projector waits behind it and shows when the "
					       "slideshow is closed."
					     : "OBS's projector covers everything while it's open, presentations too. Close it to "
					       "show the slideshow.");
		projBtn->setStyleSheet(on ? "QPushButton{background:#F5B700;color:#132932;font-weight:bold;border-radius:6px;}"
					  : "QPushButton{font-weight:bold;}");

		// The camera's status
		{
			const CamPanel cp = camPanel();
			const QString what = !cp.usb ? "Not plugged in"
					    : cp.starting ? "Starting OBSBOT Center…"
					    : !cp.running ? "OBSBOT Center is closed"
					    : cp.answering ? QString("Through OBSBOT Center · OSC port %1").arg(cp.oscPort)
							   : QString("OBSBOT Center is open but hasn't answered on OSC port %1 yet").arg(cp.oscPort);
			const bool ready = cp.err.empty();
			camStatus->setText(QString("<span style='color:%1'>●</span> <b>OBSBOT Tiny 2</b> · %2 · <b>%3</b>")
						   .arg(ready ? "#3ccf7e" : "#666")
						   .arg(what.toHtmlEscaped())
						   .arg(cp.starting ? "Starting" : ready ? "Ready" : "Offline"));
			QString warn;
			if (cp.crashed)
				warn = "OBSBOT Center closed while OBS was projecting, so the camera can't be moved. Press Open OBSBOT "
				       "Center below.";
			else if (cp.busy)
				warn = "OBS isn't getting the camera's picture. OBSBOT Center's preview is probably holding it: turn off "
				       "the preview there (the eye icon).";
			else if (cp.usb && !cp.hasSource)
				warn = "No OBS camera source for the OBSBOT was found, so phones can't show a preview. Add the camera "
				       "to the Baptism Cam scene as a Video Capture Device.";
			if (!camWarn->text().startsWith("Checking"))
				camWarn->setText(warn);
			camWarn->setStyleSheet(cp.crashed ? "color:#ff5c5c;font-weight:bold;" : "color:#ff8a3d;");
			camWarn->setVisible(!camWarn->text().isEmpty());
			camStartBtn->setVisible(cp.usb && !cp.running && !cp.starting);
			camCheckBtn->setEnabled(ready);
		}

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
						: p.mode == "hymns"      ? "HYMN SING"
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
	QLabel *hymnQr = nullptr, *hymnUrl = nullptr, *hymnScene = nullptr;
	QComboBox *screenBox = nullptr;
	void saveCam()
	{
		CamPanel p = camPanel();
		p.autoCenter = camAuto->isChecked();
		p.keepAwake = camAwake->isChecked();
		p.upside = camUpside->isChecked();
		p.topSpeed = camSpeed->value();
		p.oscPort = camPort->value();
		p.replyPort = camReply->value();
		p.centerDir = camDir->text().trimmed().toStdString();
		camSetPanel(p);
	}
	QLabel *camStatus = nullptr, *camWarn = nullptr, *camSpeedV = nullptr;
	QPushButton *camStartBtn = nullptr, *camCheckBtn = nullptr;
	QCheckBox *camAuto = nullptr, *camAwake = nullptr, *camUpside = nullptr;
	QSlider *camSpeed = nullptr;
	QLineEdit *camDir = nullptr;
	QSpinBox *camPort = nullptr, *camReply = nullptr;
	QPushButton *projBtn = nullptr, *topAuto = nullptr, *topObs = nullptr, *topPres = nullptr;
	QLabel *topNote = nullptr;

	static QString segStyle(const char *radius)
	{
		return QString("QPushButton{font-weight:bold;border-radius:%1;padding:4px 8px;}"
			       "QPushButton:checked{background:#F5B700;color:#132932;}")
			.arg(radius);
	}

	void chooseTop(const std::string &which)
	{
		setOnTop(which);
		applyOnTop();
		last.clear();
		refresh();
		refreshState();
	}
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
