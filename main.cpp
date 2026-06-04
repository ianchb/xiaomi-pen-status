#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QMenu>
#include <QPainter>
#include <QProgressBar>
#include <QPushButton>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <optional>

static constexpr const char *kDefaultSysfs =
	"/sys/devices/platform/pmic-glink/pmic_glink.power-supply.0/xiaomi";

static std::optional<int> readInt(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
		return std::nullopt;

	bool ok = false;
	const int value = QString::fromUtf8(file.readAll()).trimmed().toInt(&ok, 0);
	if (!ok)
		return std::nullopt;

	return value;
}

static bool useChinese()
{
	const QByteArray lang = qgetenv("XIAOMI_PEN_LANG").toLower();
	if (lang == "zh" || lang == "zh_cn" || lang == "cn")
		return true;
	if (lang == "en" || lang == "en_us")
		return false;

	return QLocale::system().language() == QLocale::Chinese;
}

static QString trText(const char *zh, const char *en)
{
	return QString::fromUtf8(useChinese() ? zh : en);
}

struct PenState {
	std::optional<int> hall3;
	std::optional<int> hall4;
	std::optional<int> soc;
	std::optional<int> placeErr;
	std::optional<int> txSs;
	std::optional<int> txIout;
	std::optional<int> txVout;

	bool valid() const
	{
		return hall3.has_value() || hall4.has_value() || soc.has_value();
	}

	bool placed() const
	{
		return hall3.value_or(1) == 0 || hall4.value_or(1) == 0;
	}

	bool misplaced() const
	{
		return placeErr.value_or(0) != 0;
	}

	bool batteryKnown() const
	{
		return soc.has_value() && *soc >= 0 && *soc <= 100;
	}
};

enum class VisualState {
	Unknown,
	Detached,
	Placed,
	Misplaced,
};

class PenStatusWindow : public QWidget {
public:
	PenStatusWindow()
	{
		const QByteArray envPath = qgetenv("XIAOMI_PEN_SYSFS");
		sysfsBase = envPath.isEmpty() ? QString::fromUtf8(kDefaultSysfs)
					      : QString::fromUtf8(envPath);

		setWindowTitle(trText("手写笔状态", "Stylus Status"));
		setMinimumSize(440, 420);

		statusDot = new QLabel;
		statusDot->setFixedSize(12, 12);

		titleLabel = new QLabel(trText("手写笔状态", "Stylus Status"));
		titleLabel->setObjectName(QStringLiteral("titleLabel"));

		stateLabel = new QLabel(trText("读取中", "Reading"));
		stateLabel->setObjectName(QStringLiteral("stateLabel"));

		summaryLabel = new QLabel;
		summaryLabel->setObjectName(QStringLiteral("summaryLabel"));
		summaryLabel->setWordWrap(true);

		batteryNumber = new QLabel(QStringLiteral("--"));
		batteryNumber->setObjectName(QStringLiteral("batteryNumber"));

		batteryCaption = new QLabel(trText("电量", "Battery"));
		batteryCaption->setObjectName(QStringLiteral("captionLabel"));

		batteryBar = new QProgressBar;
		batteryBar->setRange(0, 100);
		batteryBar->setTextVisible(false);
		batteryBar->setFixedHeight(10);

		warningLabel = new QLabel;
		warningLabel->setWordWrap(true);
		warningLabel->setObjectName(QStringLiteral("warningLabel"));

		debugGroup = new QGroupBox(trText("调试信息", "Debug"));
		auto *debugLayout = new QGridLayout(debugGroup);
		debugLayout->setColumnStretch(1, 1);
		addDebugRow(debugLayout, 0, QStringLiteral("pen_tx_ss"), &txSsValue);
		addDebugRow(debugLayout, 1, QStringLiteral("tx_iout"), &txIoutValue);
		addDebugRow(debugLayout, 2, QStringLiteral("tx_vout"), &txVoutValue);
		addDebugRow(debugLayout, 3, QStringLiteral("hall3 / hall4"), &hallValue);
		addDebugRow(debugLayout, 4, QStringLiteral("pen_place_err"), &placeErrValue);

		auto *refreshButton = new QPushButton(trText("刷新", "Refresh"));
		connect(refreshButton, &QPushButton::clicked, this, &PenStatusWindow::refresh);

		auto *header = new QHBoxLayout;
		header->addWidget(statusDot);
		header->addWidget(titleLabel, 1);
		header->addWidget(refreshButton);

		auto *batteryPanel = new QFrame;
		batteryPanel->setObjectName(QStringLiteral("batteryPanel"));
		auto *batteryPanelLayout = new QVBoxLayout(batteryPanel);
		batteryPanelLayout->setContentsMargins(16, 14, 16, 14);
		batteryPanelLayout->setSpacing(8);
		batteryPanelLayout->addWidget(batteryCaption);
		batteryPanelLayout->addWidget(batteryNumber);
		batteryPanelLayout->addWidget(batteryBar);

		auto *layout = new QVBoxLayout(this);
		layout->setContentsMargins(22, 22, 22, 22);
		layout->setSpacing(14);
		layout->addLayout(header);
		layout->addWidget(stateLabel);
		layout->addWidget(summaryLabel);
		layout->addWidget(batteryPanel);
		layout->addWidget(warningLabel);
		layout->addWidget(debugGroup);

		trayMenu = new QMenu(this);
		showAction = trayMenu->addAction(trText("显示", "Show"));
		quitAction = trayMenu->addAction(trText("退出", "Quit"));
		connect(showAction, &QAction::triggered, this, [this]() {
			showNormal();
			raise();
			activateWindow();
		});
		connect(quitAction, &QAction::triggered, qApp, [this]() {
			allowQuit = true;
			qApp->quit();
		});

		tray = new QSystemTrayIcon(this);
		tray->setContextMenu(trayMenu);
		tray->setIcon(makeTrayIcon(VisualState::Unknown));
		tray->setToolTip(trText("手写笔状态", "Stylus Status"));
		connect(tray, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
			if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
				isVisible() ? hide() : showNormal();
			}
		});
		tray->show();

		timer = new QTimer(this);
		timer->setInterval(1000);
		connect(timer, &QTimer::timeout, this, &PenStatusWindow::refresh);
		timer->start();

		setStyleSheet(QStringLiteral(R"(
			QWidget {
				background: #f6f5f2;
				color: #1f2328;
				font-size: 14px;
			}
			#titleLabel {
				font-size: 18px;
				font-weight: 700;
			}
			#stateLabel {
				font-size: 34px;
				font-weight: 800;
			}
			#summaryLabel {
				color: #5f656d;
				font-size: 14px;
			}
			#batteryPanel {
				background: #ffffff;
				border: 1px solid #dedbd2;
				border-radius: 8px;
			}
			#captionLabel {
				color: #74736d;
				font-size: 13px;
				font-weight: 600;
			}
			#batteryNumber {
				font-size: 42px;
				font-weight: 800;
			}
			#warningLabel {
				color: #8a4b00;
				font-weight: 700;
			}
			QProgressBar {
				border: 0;
				border-radius: 5px;
				background: #e6e3dc;
			}
			QProgressBar::chunk {
				border-radius: 5px;
				background: #1f7a5c;
			}
			QGroupBox {
				border: 1px solid #dedbd2;
				border-radius: 8px;
				margin-top: 12px;
				padding: 12px;
				background: #ffffff;
			}
			QGroupBox::title {
				subcontrol-origin: margin;
				left: 10px;
				padding: 0 4px;
				color: #65645f;
				font-weight: 600;
			}
			QPushButton {
				border: 1px solid #c8c4ba;
				border-radius: 6px;
				padding: 6px 12px;
				background: #ffffff;
			}
			QPushButton:hover {
				background: #eef4f0;
			}
		)"));

		refresh();
	}

protected:
	void closeEvent(QCloseEvent *event) override
	{
		if (allowQuit) {
			event->accept();
			return;
		}

		hide();
		event->ignore();
	}

private:
	void addDebugRow(QGridLayout *layout, int row, const QString &name, QLabel **valueLabel)
	{
		auto *nameLabel = new QLabel(name);
		nameLabel->setObjectName(QStringLiteral("captionLabel"));
		*valueLabel = new QLabel(QStringLiteral("-"));
		(*valueLabel)->setTextInteractionFlags(Qt::TextSelectableByMouse);
		layout->addWidget(nameLabel, row, 0);
		layout->addWidget(*valueLabel, row, 1);
	}

	PenState readState() const
	{
		PenState state;
		state.hall3 = readInt(sysfsBase + QStringLiteral("/pen_hall3"));
		state.hall4 = readInt(sysfsBase + QStringLiteral("/pen_hall4"));
		state.soc = readInt(sysfsBase + QStringLiteral("/pen_soc"));
		state.placeErr = readInt(sysfsBase + QStringLiteral("/pen_place_err"));
		state.txSs = readInt(sysfsBase + QStringLiteral("/pen_tx_ss"));
		state.txIout = readInt(sysfsBase + QStringLiteral("/tx_iout"));
		state.txVout = readInt(sysfsBase + QStringLiteral("/tx_vout"));
		return state;
	}

	void refresh()
	{
		const PenState state = readState();

		if (!state.valid()) {
			applyStatus(VisualState::Unknown, QStringLiteral("#9b1c1c"),
				    trText("未找到设备", "Device unavailable"),
				    trText("无法读取手写笔状态", "Unable to read stylus status"));
			batteryBar->setValue(0);
			batteryNumber->setText(QStringLiteral("--"));
			warningLabel->setText(trText("请确认 qcom_battmgr 已加载并导出了 Xiaomi 属性。",
						     "Check that qcom_battmgr is loaded and exporting Xiaomi attributes."));
			updateDebug(state);
			return;
		}

		if (state.misplaced()) {
			applyStatus(VisualState::Misplaced, QStringLiteral("#c66a00"),
				    trText("未放好", "Not seated"),
				    trText("请重新放置手写笔", "Reseat the stylus"));
			warningLabel->setText(trText("手写笔没有正确贴合充电位置。",
						     "The stylus is not aligned with the charging position."));
			notifyOnce(trText("手写笔未放好", "Stylus not seated"),
				   trText("请重新放置手写笔。", "Please reseat the stylus."));
		} else if (state.placed()) {
			applyStatus(VisualState::Placed, QStringLiteral("#1f7a5c"),
				    trText("已放回", "Docked"),
				    trText("手写笔在充电位置", "Stylus is in the charging position"));
			warningLabel->clear();
			misplacedNotified = false;
		} else {
			applyStatus(VisualState::Detached, QStringLiteral("#3867a6"),
				    trText("已取下", "Detached"),
				    trText("手写笔未在充电位置", "Stylus is away from the charging position"));
			warningLabel->clear();
			misplacedNotified = false;
		}

		if (state.batteryKnown()) {
			batteryBar->setValue(*state.soc);
			batteryNumber->setText(QStringLiteral("%1%").arg(*state.soc));
		} else {
			batteryBar->setValue(0);
			batteryNumber->setText(QStringLiteral("--"));
		}

		updateDebug(state);
	}

	void applyStatus(VisualState visualState, const QString &color, const QString &state,
			 const QString &tooltip)
	{
		statusDot->setStyleSheet(QStringLiteral("border-radius: 6px; background: %1;").arg(color));
		stateLabel->setText(state);
		summaryLabel->setText(tooltip);
		tray->setToolTip(tooltip);
		tray->setIcon(makeTrayIcon(visualState));
	}

	QIcon makeTrayIcon(VisualState state) const
	{
		QColor color;
		switch (state) {
		case VisualState::Placed:
			color = QColor(QStringLiteral("#1f7a5c"));
			break;
		case VisualState::Detached:
			color = QColor(QStringLiteral("#3867a6"));
			break;
		case VisualState::Misplaced:
			color = QColor(QStringLiteral("#c66a00"));
			break;
		case VisualState::Unknown:
		default:
			color = QColor(QStringLiteral("#8b8f94"));
			break;
		}

		QPixmap pixmap(64, 64);
		pixmap.fill(Qt::transparent);

		QPainter painter(&pixmap);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawEllipse(4, 4, 56, 56);

		QPen pen(Qt::white, 6, Qt::SolidLine, Qt::RoundCap);
		painter.setPen(pen);
		painter.drawLine(24, 42, 42, 20);
		painter.drawPoint(21, 45);
		painter.end();

		return QIcon(pixmap);
	}

	void notifyOnce(const QString &title, const QString &message)
	{
		if (misplacedNotified || !tray->isVisible())
			return;

		tray->showMessage(title, message, QSystemTrayIcon::Warning, 5000);
		misplacedNotified = true;
	}

	void updateDebug(const PenState &state)
	{
		txSsValue->setText(valueText(state.txSs));
		txIoutValue->setText(valueText(state.txIout));
		txVoutValue->setText(valueText(state.txVout));
		hallValue->setText(QStringLiteral("%1 / %2").arg(valueText(state.hall3), valueText(state.hall4)));
		placeErrValue->setText(valueText(state.placeErr));
	}

	static QString valueText(std::optional<int> value)
	{
		if (!value.has_value())
			return QStringLiteral("-");
		return QString::number(*value);
	}

	QString sysfsBase;
	QLabel *statusDot = nullptr;
	QLabel *titleLabel = nullptr;
	QLabel *stateLabel = nullptr;
	QLabel *summaryLabel = nullptr;
	QLabel *batteryNumber = nullptr;
	QLabel *batteryCaption = nullptr;
	QLabel *warningLabel = nullptr;
	QLabel *txSsValue = nullptr;
	QLabel *txIoutValue = nullptr;
	QLabel *txVoutValue = nullptr;
	QLabel *hallValue = nullptr;
	QLabel *placeErrValue = nullptr;
	QProgressBar *batteryBar = nullptr;
	QGroupBox *debugGroup = nullptr;
	QSystemTrayIcon *tray = nullptr;
	QMenu *trayMenu = nullptr;
	QAction *showAction = nullptr;
	QAction *quitAction = nullptr;
	QTimer *timer = nullptr;
	bool misplacedNotified = false;
	bool allowQuit = false;
};

int main(int argc, char **argv)
{
	QApplication app(argc, argv);
	app.setApplicationName(QStringLiteral("xiaomi-pen-status"));
	app.setDesktopFileName(QStringLiteral("xiaomi-pen-status"));
	app.setQuitOnLastWindowClosed(false);

	PenStatusWindow window;
	window.show();

	return app.exec();
}
