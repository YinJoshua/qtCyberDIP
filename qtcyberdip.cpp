#include "stdafx.h"
#include "qtcyberdip.h"
#include "ui_qtcyberdip.h"
#include "harris.h"
#include <opencv2/core/core.hpp>
#include <opencv2/highgui/highgui.hpp>
#include <opencv2/nonfree/features2d.hpp>
#include <opencv2/legacy/legacy.hpp>
#include <iostream>
#include <math.h>
#include <vector>
using namespace std;

//#include <opencv2/legacy/legacy.hpp>

#define ADB_PATH "prebuilts/adb.exe"
using namespace cv;


qtCyberDip::qtCyberDip(QWidget *parent) :
QMainWindow(parent),
ui(new Ui::qtCyberDip), bbqADBProcess(NULL), bbqDebugWidget(nullptr), bbqServiceShouldRun(false), bbqCrashCount(0),
comSPH(nullptr), comPosX(0), comPosY(0), initImg(true), hitDown(false), fetch(false)
{
	ui->setupUi(this);

	// Setup UDP discovery socket
	bbqAnnouncer = new QUdpSocket(this);
	bbqAnnouncer->bind(QHostAddress::Any, 9876);
	connect(bbqAnnouncer, SIGNAL(readyRead()), this, SLOT(bbqDiscoveryReadyRead()));

	// Connect UI slots
	connect(ui->bbqListDevices, SIGNAL(itemClicked(QListWidgetItem*)), this, SLOT(bbqSelectDevice(QListWidgetItem*)));
	connect(ui->bbqListDevices, SIGNAL(itemDoubleClicked(QListWidgetItem*)), this, SLOT(bbqDoubleClickDevice(QListWidgetItem*)));
	connect(ui->bbqConnect, SIGNAL(clicked()), this, SLOT(bbqClickConnect()));
	connect(ui->bbqBootstrapUSB, SIGNAL(clicked()), this, SLOT(bbqClickBootstrapUSB()));
	connect(ui->bbqConnectUSB, SIGNAL(clicked()), this, SLOT(bbqClickConnectUSB()));
	connect(ui->bbqCbQuality, SIGNAL(currentIndexChanged(int)), this, SLOT(bbqQualityChanged(int)));
	connect(ui->bbqSpinBitrate, SIGNAL(valueChanged(int)), this, SLOT(bbqBitrateChanged(int)));
	connect(ui->bbqDebugLog, SIGNAL(clicked()), this, SLOT(bbqClickShowDebugLog()));
	connect(ui->comInitButton, SIGNAL(clicked()), this, SLOT(comInitPara()));
	connect(ui->comConnectButton, SIGNAL(clicked()), this, SLOT(comClickConnectButton()));
	connect(ui->comSendButton, SIGNAL(clicked()), this, SLOT(comClickSendButton()));
	connect(ui->comSendEdit, SIGNAL(returnPressed()), this, SLOT(comClickSendButton()));
	connect(ui->comClcButton, SIGNAL(clicked()), this, SLOT(comClickClearButton()));
	connect(ui->comHitButton, SIGNAL(clicked()), this, SLOT(comClickHitButton()));
	connect(ui->comReturnButton, SIGNAL(clicked()), this, SLOT(comClickRetButton()));
	connect(ui->comUpButton, SIGNAL(clicked()), this, SLOT(comMoveStepUp()));
	connect(ui->comDownButton, SIGNAL(clicked()), this, SLOT(comMoveStepDown()));
	connect(ui->comLeftButton, SIGNAL(clicked()), this, SLOT(comMoveStepLeft()));
	connect(ui->comRightButton, SIGNAL(clicked()), this, SLOT(comMoveStepRight()));
	connect(ui->capClcButton, SIGNAL(clicked()), this, SLOT(capClickClearButton()));
	connect(ui->capScanButton, SIGNAL(clicked()), this, SLOT(capClickScanButton()));
	connect(ui->capStartButton, SIGNAL(clicked()), this, SLOT(capClickConnect()));
	connect(ui->capList, SIGNAL(itemDoubleClicked(QListWidgetItem*)), this, SLOT(capDoubleClickWin(QListWidgetItem*)));

	comUpdatePos();

	//监听子控件事件
	ui->comSelList->installEventFilter(this);
	//     | Who sends event &&         | Who will watch event


	startTimer(500);
}

qtCyberDip::~qtCyberDip()
{
	delete ui;
}

void qtCyberDip::closeEvent(QCloseEvent* evt)
{
	Q_UNUSED(evt);
	if (bbqADBProcess)
	{
		disconnect(bbqADBProcess, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(bbqADBProcessFinishes()));
		bbqADBProcess->terminate();
		delete bbqADBProcess;
	}
	if (bbqDebugWidget)
	{
		bbqDebugWidget->close();
	}
	if (comSPH)
	{
		comSPH->disConnect();

		delete comSPH;
	}
}

void qtCyberDip::timerEvent(QTimerEvent* evt)
{
	Q_UNUSED(evt);

	// See if we have devices that disappeared. We make them timeout after 3 seconds.
	for (auto it = bbqDevices.begin(); it != bbqDevices.end(); ++it)
	{
		if ((*it)->lastPing.elapsed() > 3000)
		{
			ui->bbqListDevices->takeItem(bbqDevices.indexOf(*it));
			delete (*it);
			it = bbqDevices.erase(it);
			break;
		}
	}
}

bool qtCyberDip::eventFilter(QObject* watched, QEvent* event)
{
	//定义点击combobox之后刷新可用COM口
	if (watched == ui->comSelList && event->type() == QEvent::MouseButtonPress)
	{
		comScanPorts();
	}
	return QObject::eventFilter(watched, event);
}

void qtCyberDip::bbqDiscoveryReadyRead()
{
	QByteArray datagram;
	QHostAddress sender;
	quint16 senderPort;

	while (bbqAnnouncer->hasPendingDatagrams())
	{
		if (datagram.size() != bbqAnnouncer->pendingDatagramSize())
			datagram.resize(bbqAnnouncer->pendingDatagramSize());

		// Read pending UDP datagram
		bbqAnnouncer->readDatagram(datagram.data(), datagram.size(),
			&sender, &senderPort);

		// Format of announcer packet:
		// 0 : Protocol version
		// 1 : Device name size
		// 2+: Device name

		unsigned char protocolVersion = datagram.at(0),
			deviceNameSize = datagram.at(1);

		QString deviceName = QByteArray(datagram.data() + 2, deviceNameSize);
		QString remoteIp = sender.toString();

		// Make sure we don't already know this device
		bool exists = false;
		for (auto it = bbqDevices.begin(); it != bbqDevices.end(); ++it)
		{
			if ((*it)->name == deviceName && (*it)->address == remoteIp)
			{
				(*it)->lastPing.restart();
				exists = true;
				break;
			}
		}

		if (!exists)
		{
			// XXX: Protocol v3 indicates that audio can't be streamed, and v4
			// indicates that we can stream audio. However, the user can choose
			// to turn off audio even on v4. Maybe in the future we could indicate
			// that.
			Device* device = new Device;
			device->name = deviceName;
			device->address = remoteIp;
			device->lastPing.start();

			ui->bbqListDevices->addItem(QString("%1 - (%2)").arg(deviceName, remoteIp));
			bbqDevices.push_back(device);
		}
	}
}
void qtCyberDip::bbqClickConnect()
{
	setCursor(Qt::WaitCursor);
	// Check that the IP entered is valid
	QString ip = ui->bbqIP->text();
	QHostAddress address(ip);
	if (QAbstractSocket::IPv4Protocol != address.protocol())
	{
		QMessageBox::critical(this, "Invalid IP", "The IP address you entered is invalid");
		setCursor(Qt::ArrowCursor);
		return;
	}

	// The IP is valid, connect to there
	bbqScreenForm* screen = new bbqScreenForm(this);
	connect(screen, SIGNAL(imgReady(QImage)), this, SLOT(processImg(QImage)));
	screen->setAttribute(Qt::WA_DeleteOnClose);
	screen->setShowFps(ui->bbqShowFps->isChecked());
	screen->show();
	screen->connectTo(ui->bbqIP->text());

	// Hide this dialog
	hide();
	setCursor(Qt::ArrowCursor);
}
void qtCyberDip::bbqSelectDevice(QListWidgetItem* item)
{
	Q_UNUSED(item);

	int index = ui->bbqListDevices->currentRow();
	if (index >= 0)
	{
		ui->bbqIP->setText(bbqDevices.at(index)->address);
	}
}

void qtCyberDip::bbqDoubleClickDevice(QListWidgetItem* item)
{
	bbqSelectDevice(item);
	bbqClickConnect();
}

QProcess* qtCyberDip::bbqRunAdb(const QStringList& params)
{
	QProcess* process = new QProcess(this);

	connect(process, SIGNAL(readyReadStandardOutput()), this, SLOT(bbqADBProcessReadyRead()));
	connect(process, SIGNAL(readyReadStandardError()), this, SLOT(bbqADBErrorReadyRead()));

#ifndef PLAT_APPLE
	process->start(ADB_PATH, params);
#else
	process->start(QDir(QCoreApplication::applicationDirPath()).absolutePath() + "/" + ADB_PATH, params);
#endif	

	return process;
}

void qtCyberDip::bbqClickBootstrapUSB()
{
	//qDebug()<< bbqADBProcess;
	if (!bbqServiceShouldRun)
	{
		bbqCrashCount = 0;
		bbqServiceShouldRun = true;
		bbqStartUsbService();
	}
	else
	{
		bbqServiceShouldRun = false;
		if (bbqADBProcess)
		{
			bbqADBProcess->terminate();
			bbqADBProcess->kill();
			//qDebug() << bbqADBProcess;
		}
	}
}

void qtCyberDip::bbqClickConnectUSB()
{
	// Forward TCP port to localhost and connect to it
	QStringList args;
	args << "forward";
	args << "tcp:9876";
	args << "tcp:9876";

	bbqRunAdb(args);

	ui->bbqIP->setText("127.0.0.1");
	bbqClickConnect();
}

void qtCyberDip::bbqADBProcessFinishes()
{
	//qDebug() << "finish";
	if (bbqServiceShouldRun)
	{
		bbqCrashCount++;

		if (bbqCrashCount > 20)
		{
			QMessageBox::critical(this, "Crash!", "It appears that the streaming process has crashed over 20 times. Please check the debug log window and send a screenshot to the support.");
			bbqServiceShouldRun = false;
		}
		// If the process crashed, reboot it
		bbqStartUsbService();
	}
	else
	{
		//qDebug() << "Normal end";
		// Normal stop
		ui->bbqBootstrapUSB->setText("Start USB service");
		ui->bbqConnectUSB->setEnabled(false);
	}
}

void qtCyberDip::bbqADBProcessReadyRead()
{
	//qDebug() << "ReadyRead";
	QProcess* process = (QProcess*)QObject::sender();

	QByteArray stdOut = process->readAllStandardOutput();
	QString stdOutLine = QString(stdOut).trimmed();

	if (stdOutLine.contains("/data/data") && stdOutLine.contains("No such file or directory"))
	{
		bbqServiceShouldRun = false;
		bbqServiceStartError = true;
	}
	else if (stdOutLine.contains("Unable to chmod"))
	{
		bbqServiceShouldRun = false;
		bbqServiceStartError = true;
	}

	if (!stdOutLine.isEmpty())
	{
		bbqADBLog.push_back(stdOutLine);

		if (bbqDebugWidget != nullptr)
		{
			bbqDebugWidget->addItem(stdOutLine);
		}
	}
}

void qtCyberDip::bbqADBErrorReadyRead()
{
	QProcess* process = (QProcess*)QObject::sender();

	QByteArray stdErr = process->readAllStandardError();
	QString stdErrLine = QString(stdErr).trimmed();

	if (stdErrLine.contains("device not found"))
	{
		bbqServiceShouldRun = false;
		QMessageBox::critical(this, "Device not found or unplugged", "Cannot find an Android device connected via ADB. Make sure USB Debugging is enabled on your device, and that the ADB drivers are installed. Follow the guide on our website for more information.");
	}
	else if (stdErrLine.contains("device offline"))
	{
		bbqServiceShouldRun = false;
		QMessageBox::critical(this, "Device offline", "An Android device is connected but reports as offline. Check your device for any additional information, or try to unplug and replug your device");
	}
	else if (stdErrLine.contains("unauthorized"))
	{
		bbqServiceShouldRun = false;
		QMessageBox::critical(this, "Device unauthorized", "An Android device is connected but reports as unauthorized. Please check the confirmation dialog on your device.");
	}

	if (!stdErrLine.isEmpty())
	{
		bbqADBErrorLog.push_back(stdErrLine);

		if (bbqDebugWidget != nullptr)
		{
			QListWidgetItem* item = new QListWidgetItem(stdErrLine);
			item->setTextColor(QColor(255, 0, 0));
			bbqDebugWidget->addItem(item);
		}
	}
	bbqADBProcess->terminate();
	bbqADBProcess->kill();

}

void qtCyberDip::bbqStartUsbService()
{
	bbqServiceStartError = false;

	ui->bbqBootstrapUSB->setEnabled(false);
	ui->bbqBootstrapUSB->setText("Starting...");


	setCursor(Qt::WaitCursor);
	qApp->processEvents();

	if (!bbqADBProcess)
	{
		bbqADBProcess = new QProcess(this);
		connect(bbqADBProcess, SIGNAL(finished(int, QProcess::ExitStatus)), this, SLOT(bbqADBProcessFinishes()));
		connect(bbqADBProcess, SIGNAL(readyReadStandardOutput()), this, SLOT(bbqADBProcessReadyRead()));
		connect(bbqADBProcess, SIGNAL(readyReadStandardError()), this, SLOT(bbqADBErrorReadyRead()));
	}

	// Copy binary to workaround some security restrictions on Lollipop and Knox
	QStringList args;
	args << "shell";
	args << "cp";
	args << "/data/data/org.bbqdroid.bbqscreen/files/bbqscreen";
	args << "/data/local/tmp/bbqscreen";
	QProcess* copyProc = bbqRunAdb(args);
	copyProc->waitForFinished();
	if (bbqServiceStartError)
	{
		QMessageBox::critical(this, "Unable to prepare the USB service", "Unable to copy the BBQScreen service to an executable zone on your device, as it hasn't been found. Please make sure the BBQScreen app is installed, and that you opened it once, and pressed 'USB' if prompted or turned it on once.");
		delete copyProc;
		return;
	}

	args.clear();
	args << "shell";
	args << "chmod";
	args << "755";
	args << "/data/local/tmp/bbqscreen";
	QProcess* chmodProc = bbqRunAdb(args);
	chmodProc->waitForFinished();
	if (bbqServiceStartError)
	{
		QMessageBox::critical(this, "Unable to prepare the USB service", "Unable to set the permissions of the BBQScreen service to executable. Please contact support.");
		delete chmodProc;
		return;
	}

	args.clear();
	args << "shell";
	args << "/data/local/tmp/bbqscreen";
	args << "-s";
	args << "50";
	switch (ui->bbqCbQuality->currentIndex())
	{
	case 0:
		args << "-1080";
		break;
	case 1:
		args << "-720";
		break;
	case 2:
		args << "-540";
		break;
	case 3:
		args << "-360";
		break;
	}
	args << "-q";
	args << QString::number(ui->bbqSpinBitrate->value());
	args << "-i";

	bbqADBProcess->start(ADB_PATH, args);
	ui->bbqConnectUSB->setEnabled(true);
	ui->bbqBootstrapUSB->setEnabled(true);
	ui->bbqBootstrapUSB->setText("Stop USB service");
	setCursor(Qt::ArrowCursor);
	delete chmodProc;
	delete copyProc;
}

void qtCyberDip::bbqQualityChanged(int index)
{
	if (bbqADBProcess)
	{
		bbqCrashCount = 0;
		// Restart the app
		bbqADBProcess->terminate();
		bbqADBProcess->kill();
	}
}

void qtCyberDip::bbqBitrateChanged(int value)
{
	if (bbqADBProcess)
	{
		bbqCrashCount = 0;
		// Restart the app
		bbqADBProcess->terminate();
		bbqADBProcess->kill();
	}
}
void qtCyberDip::bbqClickShowDebugLog()
{
	if (bbqDebugWidget != nullptr) {
		delete bbqDebugWidget;
	}
	bbqDebugWidget = new QListWidget();
	bbqDebugWidget->addItems(bbqADBLog);

	for (auto it = bbqADBErrorLog.begin(); it != bbqADBErrorLog.end(); ++it) {
		QListWidgetItem* item = new QListWidgetItem(*it);
		item->setTextColor(QColor(255, 0, 0));
		bbqDebugWidget->addItem(item);
	}

	bbqDebugWidget->show();

}

void qtCyberDip::comInitPara()
{
	/* 0.8c*/
	/*
	$0=250.000 (x, step/mm)
	$1=250.000 (y, step/mm)
	$2=250.000 (z, step/mm)
	$3=10 (step pulse, usec)
	$4=50.000 (default feed, mm/min)
	$5=2000.000 (default seek, mm/min)
	$6=192 (step port invert mask, int:11000000)
	$7=25 (step idle delay, msec)
	$8=120.000 (acceleration, mm/sec^2)
	$9=0.050 (junction deviation, mm)
	$10=0.100 (arc, mm/segment)
	$11=25 (n-arc correction, int)
	$12=3 (n-decimals, int)
	$13=0 (report inches, bool)
	$14=1 (auto start, bool)
	$15=0 (invert step enable, bool)
	$16=0 (hard limits, bool)
	$17=0 (homing cycle, bool)
	$18=0 (homing dir invert mask, int:00000000)
	$19=25.000 (homing feed, mm/min)
	$20=250.000 (homing seek, mm/min)
	$21=100 (homing debounce, msec)
	$22=1.000 (homing pull-off, mm)

	*/

	/*	0.9j */
	/*$0=10 (step pulse, usec)
	$1=25 (step idle delay, msec)
	$2=0 (step port invert mask:00000000)
	$3=0 (dir port invert mask:00000000)
	$4=0 (step enable invert, bool)
	$5=0 (limit pins invert, bool)
	$6=0 (probe pin invert, bool)
	$10=3 (status report mask:00000011)
	$11=0.010 (junction deviation, mm)
	$12=0.002 (arc tolerance, mm)
	$13=0 (report inches, bool)
	$20=0 (soft limits, bool)
	$21=0 (hard limits, bool)
	$22=0 (homing cycle, bool)
	$23=0 (homing dir invert mask:00000000)
	$24=25.000 (homing feed, mm/min)
	$25=500.000 (homing seek, mm/min)
	$26=250 (homing debounce, msec)
	$27=1.000 (homing pull-off, mm)
	$100=250.000 (x, step/mm)
	$101=250.000 (y, step/mm)
	$102=250.000 (z, step/mm)
	$110=2500.000 (x max rate, mm/min)
	$111=2500.000 (y max rate, mm/min)
	$112=500.000 (z max rate, mm/min)
	$120=120.000 (x accel, mm/sec^2)
	$121=120.000 (y accel, mm/sec^2)
	$122=10.000 (z accel, mm/sec^2)
	$130=200.000 (x max travel, mm)
	$131=200.000 (y max travel, mm)
	$132=200.000 (z max travel, mm)*/
	QList<QPair<int, float>> para;
	if (ui->comCheckS->isChecked())
	{

		para.push_back(qMakePair(0, 10));
		para.push_back(qMakePair(1, 25));
		para.push_back(qMakePair(2, 0));
		para.push_back(qMakePair(3, 0));
		para.push_back(qMakePair(4, 0));
		para.push_back(qMakePair(5, 0));
		para.push_back(qMakePair(6, 0));

		para.push_back(qMakePair(10, 3));
		para.push_back(qMakePair(11, 0.010));
		para.push_back(qMakePair(12, 0.002));
		para.push_back(qMakePair(13, 0));

		para.push_back(qMakePair(20, 0));
		para.push_back(qMakePair(21, 0));
		para.push_back(qMakePair(22, 0));
		para.push_back(qMakePair(23, 0));
		para.push_back(qMakePair(24, 25));
		para.push_back(qMakePair(25, 500));
		para.push_back(qMakePair(26, 250));
		para.push_back(qMakePair(27, 1));

		para.push_back(qMakePair(100, 250));
		para.push_back(qMakePair(101, 250));
		para.push_back(qMakePair(102, 250));

		para.push_back(qMakePair(110, 2500));
		para.push_back(qMakePair(111, 2500));
		para.push_back(qMakePair(112, 500));

		para.push_back(qMakePair(120, 120));
		para.push_back(qMakePair(121, 120));
		para.push_back(qMakePair(122, 10));

		para.push_back(qMakePair(130, 200));
		para.push_back(qMakePair(131, 200));
		para.push_back(qMakePair(132, 200));
	}
	else
	{
		para.push_back(qMakePair(0, 250));
		para.push_back(qMakePair(1, 250));
		para.push_back(qMakePair(2, 200));
		para.push_back(qMakePair(3, 10));
		para.push_back(qMakePair(4, 50));
		para.push_back(qMakePair(5, 2000));
		para.push_back(qMakePair(6, 192));
		para.push_back(qMakePair(7, 25));
		para.push_back(qMakePair(8, 120));
		para.push_back(qMakePair(9, 0.05));
		para.push_back(qMakePair(10, 0.1));
		para.push_back(qMakePair(11, 25));
		para.push_back(qMakePair(12, 3));
		para.push_back(qMakePair(13, 0));
		para.push_back(qMakePair(14, 1));
		para.push_back(qMakePair(15, 0));
		para.push_back(qMakePair(16, 0));
		para.push_back(qMakePair(17, 0));
		para.push_back(qMakePair(18, 0));
		para.push_back(qMakePair(19, 25));
		para.push_back(qMakePair(20, 250));
		para.push_back(qMakePair(21, 100));
		para.push_back(qMakePair(22, 1.0));

	}

	QList<QPair<int, float>>::iterator tp = para.begin();
	while (tp != para.end())
	{
		char cache[64];
		sprintf(cache, "$%d=%0.3f", tp->first, tp->second);
		comRequestToSend(QString(cache));
		tp++;
		Sleep(500);
	}

}

void qtCyberDip::comClickConnectButton()
{
	setCursor(Qt::WaitCursor);
	bool built = comSPH;
	bool online = false;
	if (built){ online = comSPH->isOpen(); }
	if (online)
	{
		comLogAdd("Disconnecting..", 2);
		comSPH->disConnect();
		if (!(comSPH->isOpen())){ comLogAdd("Done.", 2); }
		ui->comSelList->setCurrentIndex(-1);
	}
	else
	{
		int index = ui->comSelList->currentIndex();
		if (index >= 0 && index<comPorts.length())
		{
			comLogAdd("Connecting..", 2);
			if (!built)
			{
				comSPH = new comSPHandler(this);
				connect(comSPH, SIGNAL(serialPortSignals(QString, int)), this, SLOT(comLogAdd(QString, int)));
			}
			comSPH->setPort(comPorts[index]);

			if (comSPH->connectTo((ui->comCheckS->isChecked()) ? QSerialPort::Baud115200 : QSerialPort::Baud9600))
			{
				comLogAdd("Done.", 2);
			}
			else
			{
				comLogAdd("Failed", 2);
			}
		}
	}
	comUpdateUI();
	setCursor(Qt::ArrowCursor);
}

void qtCyberDip::comUpdateUI()
{
	bool online = comSPH;
	if (online){ online = comSPH->isOpen(); }
	ui->comConnectButton->setText((online) ? "Disconnect" : "Connect");
	ui->comSendButton->setEnabled(online);
	ui->comHitButton->setEnabled(online);
	ui->comHitButton->setEnabled(online);
	ui->comReturnButton->setEnabled(online);
	ui->comUpButton->setEnabled(online);
	ui->comDownButton->setEnabled(online);
	ui->comLeftButton->setEnabled(online);
	ui->comRightButton->setEnabled(online);
	ui->comSelList->setEnabled(!online);
}

void qtCyberDip::comUpdatePos()
{
	ui->comPosLabel->setText("X: " + QString::number(comPosX) + "\nY: " + QString::number(comPosY));
}


void  qtCyberDip::comLogAdd(QString txt, int type = 0)
{
	//0 -normal&receive
	//1 -send
	//2 -system
	switch (type)
	{
	case 1:ui->comMainLog->append(">>" + txt + "\n"); break;
	case 2:ui->comMainLog->append("/***   " + txt + "   ***/"); break;
	default:
		ui->comMainLog->insertPlainText(txt);
		break;
	}
	ui->comMainLog->moveCursor(QTextCursor::End);
}

void qtCyberDip::comScanPorts()
{
	ui->comSelList->clear();
	comPorts.clear();
	foreach(const QSerialPortInfo &info, QSerialPortInfo::availablePorts())
	{
		//qDebug() << "Name        : " << info.portName();
		//qDebug() << "Description : " << info.description();
		//qDebug() << "Manufacturer: " << info.manufacturer();
		ui->comSelList->addItem(info.portName() + " " + info.description());
		comPorts.push_back(info);
	}
}

void qtCyberDip::comMoveTo(double x, double y)
{
	if (x == comPosX && y == comPosY){ return; }
	comPosX = x;
	comPosY = y;
	comRequestToSend("G90");//绝对坐标
	comRequestToSend("G0 X" + QString::number(-comPosX) + " Y" + QString::number(-comPosY));
	comUpdatePos();
}

void qtCyberDip::comMoveToScale(double ratioX, double ratioY)
{
	double px = std::max(std::min(ratioX, 1.0), 0.0);
	double py = std::max(std::min(ratioY, 1.0), 0.0);
	comMoveTo(-px*RANGE_X, py*RANGE_Y);
}

void qtCyberDip::comdrag(double ratioX, double ratioY)
{
	double px = std::max(std::min(ratioX, 1.0), 0.0);
	double py = std::max(std::min(ratioY, 1.0), 0.0);
	comHitDown();
	comMoveTo(-px*RANGE_X, py*RANGE_Y);
	comHitUp();
}


void  qtCyberDip::comClickSendButton()
{
	if (ui->comSendEdit->text().length() > 0)
	{
		comRequestToSend(ui->comSendEdit->text());
	}
}

void qtCyberDip::comClickClearButton()
{
	ui->comMainLog->clear();
	ui->comSendEdit->clear();
}

void qtCyberDip::comClickHitButton()
{
	comHitDown();
	comRequestToSend("G91");//相对坐标
	//用不存在的Z轴实现延时功能
	if (fetch)
	{
		comRequestToSend("G1 Z-0.01 F5.");
	}
	else
	{
		comRequestToSend("G1 Z0.01 F5.");
	}
	fetch = !fetch;
	comHitUp();
}

void qtCyberDip::comClickRetButton()
{
	comHitUp();
	comMoveTo(0, 0);
}

void qtCyberDip::comRequestToSend(QString txt)
{
	if (!comSPH){ return; }
	comSPH->requestToSend(txt);
}

void qtCyberDip::comMoveStepUp()
{
	comMoveTo(comPosX, comPosY - ui->comSpinBox->value());

}

void qtCyberDip::comMoveStepDown()
{
	comMoveTo(comPosX, comPosY + ui->comSpinBox->value());
}
void qtCyberDip::comMoveStepLeft()
{
	comMoveTo(comPosX - ui->comSpinBox->value(), comPosY);
}
void qtCyberDip::comMoveStepRight()
{

	comMoveTo(comPosX + ui->comSpinBox->value(), comPosY);
}

void qtCyberDip::comHitDown()
{
	hitDown = true;
	comRequestToSend("M3 S1000");
}
void qtCyberDip::comHitUp()
{
	hitDown = false;
	comRequestToSend("M5");
}


void qtCyberDip::capClickClearButton()
{
	ui->capList->clear();
	capWins.clear();
}

void qtCyberDip::capClickScanButton()
{

	capClickClearButton();
	EnumWindows(capEveryWindowProc, (LPARAM) this);
}

void qtCyberDip::capAddhWnd(HWND hWnd, QString nameToShow)
{
	capWins.push_back(hWnd);
	ui->capList->addItem(nameToShow);
}

BOOL CALLBACK capEveryWindowProc(HWND hWnd, LPARAM parameter)
{
	// 不可见、不可激活的窗口不作考虑。
	if (!IsWindowVisible(hWnd)){ return true; }
	if (!IsWindowEnabled(hWnd)){ return true; }
	// 弹出式窗口不作考虑。
	LONG gwl_style = GetWindowLong(hWnd, GWL_STYLE);
	if ((gwl_style & WS_POPUP) && !(gwl_style & WS_CAPTION)){ return true; }

	// 父窗口是可见或可激活的窗口不作考虑。
	HWND hParent = (HWND)GetWindowLong(hWnd, GW_OWNER);
	if (IsWindowEnabled(hParent)){ return true; }
	if (IsWindowVisible(hParent)){ return true; }

	wchar_t szCaption[500];
	::GetWindowText(hWnd, szCaption, sizeof(szCaption));
	//if (wcslen(szCaption) <= 0){ return true; }
	((qtCyberDip*)parameter)->capAddhWnd(hWnd, "0x" + QString::number((uint)hWnd, 16) + "  " + QString::fromWCharArray(szCaption));
	return true;
}

void qtCyberDip::capClickConnect()
{

	int index = ui->capList->currentRow();
	if (index>capWins.size() - 1 || index<0){ return; }
	setCursor(Qt::WaitCursor);
	qDebug() << "Windows Handle: " << capWins[index];
	capScreenForm* screen = new capScreenForm(this);
	connect(screen, SIGNAL(imgReady(QImage)), this, SLOT(processImg(QImage)));
	screen->capSetHWND(capWins[index]);
	screen->show();
	hide();
	capClickClearButton();
	screen->capStart();
	setCursor(Qt::ArrowCursor);
}

void qtCyberDip::capDoubleClickWin(QListWidgetItem* item)
{
	capClickConnect();
}

//STEP3:替换这里的图像处理代码
//每当收到一张图片时都会调用该函数
int a = 0;
int correct[4];//检测每一块图片是否拼好

int Aindex = 0;
void qtCyberDip::processImg(QImage img)
{
	//*************************************************************//
	//                                                             //
	//                                                             //
	//         TODO:使用你的代码替换下面的图像处理代码             //
	//                                                             //
	//          建议使用comMoveToScale(x,y)发送控制指令            //
	//                                                             //
	//*************************************************************//
#ifdef VIA_OPENCV
	cv::String winName = "YIN";

	//初始化
	if (initImg)
	{
		cv::namedWindow(winName);
		cvSetMouseCallback(winName.c_str(), mouseCallback, (void*)&(argM));
		initImg = false;
	}

	cv::Mat  pt = QImage2cvMat(img);
	cv::Mat  image;
	cv::imshow(winName, pt);
	cv::Size imgSize = pt.size();
	//void findPuzzle(cv::Mat &pt, int &point_x, int &point_y);//图片不引用传递，防止画花原图片
	//int a, b;
	//findPuzzle(pt, a, b);


	cv::Mat imagex = pt;//image1在左边
	cv::Mat image0;
	switch (Aindex)
	{
	case 0:image0 = imread("C:\\Users\\DELL\\Desktop\\1.jpg"); break;
	case 1:image0 = imread("C:\\Users\\DELL\\Desktop\\2.jpg"); break;
	case 2:image0 = imread("C:\\Users\\DELL\\Desktop\\3.jpg"); break;
	case 3:image0 = imread("C:\\Users\\DELL\\Desktop\\4.jpg"); break;
	case 4:image0 = imread("C:\\Users\\DELL\\Desktop\\5.jpg"); break;
	case 5:image0 = imread("C:\\Users\\DELL\\Desktop\\6.jpg"); break;
	case 6:image0 = imread("C:\\Users\\DELL\\Desktop\\7.jpg"); break;
	case 7:image0 = imread("C:\\Users\\DELL\\Desktop\\8.jpg"); break;
	case 8:image0 = imread("C:\\Users\\DELL\\Desktop\\9.jpg"); break;
	}
	cout << Aindex << endl;
	cv::imshow("ceshi", image0);

	//检测surf特征点
	vector<KeyPoint> keypointsx, keypoints0, keypoints1, keypoints2, keypoints3;
	SurfFeatureDetector detector(50);
	detector.detect(imagex, keypointsx);
	detector.detect(image0, keypoints0);


	// 描述surf特征点
	SurfDescriptorExtractor surfDesc;
	cv::Mat descriptrosx, descriptros0, descriptros1, descriptros2, descriptros3;
	surfDesc.compute(imagex, keypointsx, descriptrosx);
	surfDesc.compute(image0, keypoints0, descriptros0);


	// 计算匹配点数
	//BruteForceMatcher<L2<float>>matcher;
	FlannBasedMatcher matcher;
	vector<DMatch> matches;
	matcher.match(descriptros0, descriptrosx, matches);
	cv::Mat imageMatches;
	cv::drawMatches(image0, keypoints0, imagex, keypointsx, matches,
		imageMatches, Scalar(255, 0, 0));
	//画出匹配图
	cv::namedWindow("匹配");
	cv::imshow("匹配", imageMatches);





	int sum_x_1 = 0;
	int sum_y_1 = 0;
	//左边小图为1
	int sum_x_2 = 0;
	int sum_y_2 = 0;
	//右边大图为2

	vector<int>asinn, f;
	for (int i = 0; i<matches.size(); i++)
	{
		double k = 0;
		int kp1_idx = matches[i].queryIdx;  //取得匹配到keypoint1的索引
		int kp2_idx = matches[i].trainIdx;   //取得匹配到keypoint2的索引
		//cout << keypoint1[kp1_idx].pt.x << "," << keypoint1[kp1_idx].pt.y;
		//cout << keypoint2[kp2_idx].pt.x << "," << keypoint2[kp2_idx].pt.y << endl;
		k = (keypointsx[kp2_idx].pt.y - keypoints0[kp1_idx].pt.y) / (keypointsx[kp2_idx].pt.x - keypoints0[kp1_idx].pt.x);
		asinn.push_back(24 * atan(k));
	}
	f = asinn;
	sort(asinn.begin(), asinn.end());
	int majority = 0;
	//求出向量众数majority
	//cout << middle;
	int len = asinn.size();
	int MaxCount = 1;
	int l = 0;
	int index = 0;

	while (l < len - 1)//遍历  
	{
		int count = 1;
		int j;
		for (j = l; j < len - 1; j++)
		{
			if (f[j] == f[j + 1])//存在连续两个数相等，则众数+1  
			{
				count++;
			}
			else
			{
				break;
			}
		}
		if (MaxCount < count)
		{
			MaxCount = count;//当前最大众数  
			index = j;//当前众数标记位置  
		}
		++j;
		l = j;//位置后移到下一个未出现的数字  
	}
	majority = f[index];
	//求出K的众数majority
	int number = 0;
	for (int j = 0; j <asinn.size(); j++)
	{
		int kp1_idx = matches[j].queryIdx;  //取得匹配到keypoint1的索引
		int kp2_idx = matches[j].trainIdx;   //取得匹配到keypoint2的索引
		if (f[j] == majority)
		{
			sum_x_1 = sum_x_1 + keypoints0[kp1_idx].pt.x;
			sum_y_1 = sum_y_1 + keypoints0[kp1_idx].pt.y;
			sum_x_2 = sum_x_2 + keypointsx[kp2_idx].pt.x;
			sum_y_2 = sum_y_2 + keypointsx[kp2_idx].pt.y;
			number++;
			//cout << "k" << j << ":"<<k[j];
		}
	}
	int x_1 = sum_x_1 / number;
	int y_1 = sum_y_1 / number;
	//特征点“重心”
	int dx = x_1 - 70.7;
	int dy = y_1 - 70.7;
	//特征点“重心”跟几何中心偏差
	int x = sum_x_2 / number - dx;
	int y = sum_y_2 / number - dy;
	//cv::waitKey(1000);
	//大图特征点坐标
	//int b = a % 2;
	//if (b == 0) { x = -1; y = -1; }
	//a++;
	//cout << a;
	//对于不知道为什么会出现的每次操作都执行两次的bug，暂时强制它只执行一次



	argM.box.x = x; argM.box.y = y;
	int origin_x, origin_y;
	switch (Aindex)
	{
	case 0:origin_x = 129.7; origin_y = 256.7; break;
	case 1:origin_x = 271; origin_y = 256.7; break;
	case 2:origin_x = 412.3; origin_y = 266.7; break;
	case 3:origin_x = 129.7; origin_y = 398; break;
	case 4:origin_x = 271; origin_y = 398; break;
	case 5:origin_x = 412.3; origin_y = 398; break;
	case 6:origin_x = 129.7; origin_y = 539.4; break;
	case 7:origin_x = 271; origin_y = 539.4; break;
	case 8:origin_x = 412.3; origin_y = 539.4; break;

	}

	if (argM.box.x >= 0 && argM.box.x < imgSize.width&&
		argM.box.y >= 0 && argM.box.y < imgSize.height
		)
	{

		qDebug() << "X:" << argM.box.x << " Y:" << argM.box.y;//输出点的坐标
		if (argM.Hit){ comHitDown(); }

		comMoveToScale(((double)argM.box.y + argM.box.width - UP_CUT) / pt.rows, ((double)argM.box.x + argM.box.height) / pt.cols);//移动到大图特征点中心
		argM.box.x = origin_x; argM.box.y = origin_y;//原图坐标修正
		comdrag(((double)argM.box.y + argM.box.width - UP_CUT) / pt.rows, ((double)argM.box.x + argM.box.height) / pt.cols);//拖动到原图位置
		argM.box.x = -1; argM.box.y = -1;

		//检测是否点击了hitbutton按钮，点了就触控笔向下点一次
		if (argM.Hit){ comHitUp(); }
		else{ comClickHitButton(); }

	}
	cout << "duoshao" << (x - origin_x)*(x - origin_x) + (y - origin_y)*(y - origin_y) << "/";
	//if ((x - origin_x)*(x - origin_x) + (y - origin_y)*(y - origin_y) < 250)
	Aindex++;
	if (Aindex == 9) Aindex = 0;








#endif



}



#ifdef VIA_OPENCV
cv::Mat qtCyberDip::QImage2cvMat(QImage image)
{
	cv::Mat mat;
	//qDebug() << image.format();
	switch (image.format())
	{
	case QImage::Format_ARGB32:
	case QImage::Format_RGB32:
	case QImage::Format_ARGB32_Premultiplied:
		mat = cv::Mat(image.height(), image.width(), CV_8UC4, (void*)image.constBits(), image.bytesPerLine());
		break;
	case QImage::Format_RGB888:
		mat = cv::Mat(image.height(), image.width(), CV_8UC3, (void*)image.constBits(), image.bytesPerLine());
		cv::cvtColor(mat, mat, CV_BGR2RGB);
		break;
	case QImage::Format_Indexed8:
		mat = cv::Mat(image.height(), image.width(), CV_8UC1, (void*)image.constBits(), image.bytesPerLine());
		break;
	}
	return mat;
}


void mouseCallback(int event, int x, int y, int flags, void*param)
{
	MouseArgs* m_arg = (MouseArgs*)param;
	switch (event)
	{
	case CV_EVENT_MOUSEMOVE: // 鼠标移动时
	{
								 if (m_arg->Drawing)
								 {
									 m_arg->box.width = x - m_arg->box.x;
									 m_arg->box.height = y - m_arg->box.y;
								 }
	}
		break;
	case CV_EVENT_LBUTTONDOWN:case CV_EVENT_RBUTTONDOWN: // 左/右键按下
	{
								  m_arg->Hit = event == CV_EVENT_RBUTTONDOWN;
								  m_arg->Drawing = true;
								  m_arg->box = cvRect(x, y, 0, 0);
	}
		break;
	case CV_EVENT_LBUTTONUP:case CV_EVENT_RBUTTONUP: // 左/右键弹起
	{
								m_arg->Hit = false;
								m_arg->Drawing = false;
								if (m_arg->box.width<0)
								{
									m_arg->box.x += m_arg->box.width;
									m_arg->box.width *= -1;
								}
								if (m_arg->box.height<0)
								{
									m_arg->box.y += m_arg->box.height;
									m_arg->box.height *= -1;
								}
	}
		break;
	}
}

void qtCyberDip::closeCV()
{
	initImg = true;
	cv::destroyAllWindows();
	comClickRetButton();
}
#endif

void findPuzzle(cv::Mat &pt, int &point_x, int &point_y)//图片不引用传递，防止画花原图片
{
	cv::Mat greypt;
	cv::Mat threshold_pt;
	cv::cvtColor(pt, greypt, CV_BGR2GRAY);
	cv::threshold(greypt, threshold_pt, 45, 255, CV_THRESH_BINARY);//可以用RGB值进行细分，先用着灰度区别吧

	int high = pt.rows;//图片大小
	int width = pt.cols;
	int high_findwindow = width*0.05;//搜索窗口大小,做成正方形吧
	int width_findwindow = width*0.05;
	int boundary_up = high*0.26;//搜索边界大小//自己测量下
	int boudary_down = high*0.78;
	int target_centerx = 0;
	int target_centery = 0;
	bool found = false;
	//qDebug() << "start process" << high << width;
	//cv::waitKey(1000);//进入处理函数


	cv::line(pt, cv::Point(0, boundary_up), cv::Point(width, boundary_up), cv::Scalar(255, 0, 0));//画上边界
	cv::line(pt, cv::Point(0, boudary_down), cv::Point(width, boudary_down), cv::Scalar(255, 0, 0));

	//开始搜索
	for (int i = 0; i + width_findwindow < width; i = i + width_findwindow)//起始点要刨掉顶部边框，终止点要防止越界
	{
		for (int j = int(0.08*high); j + high_findwindow < boundary_up; j = j + high_findwindow)//小窗遍历搜索区
		{
			//qDebug() << j << "," << i << endl;
			int count_white = 0;//记录小窗内的白点数（有效区域）
			for (int i1 = 0; i1 < high_findwindow; i1++)//小窗内遍历统计
			{
				uchar* value = threshold_pt.ptr<uchar>(i1 + j);//取指针
				for (int j1 = 0; j1 < width_findwindow; j1++)
				{
					if (value[j1 + i] == 255) count_white++;
				}
			}
			if ((count_white / (width_findwindow*high_findwindow)) > 0.9)//判断搜索结束
			{
				target_centerx = i + width_findwindow / 2;
				target_centery = j + high_findwindow / 2;
				found = true;
				//cv::waitKey(1000);//搜索成功
			}
			if (found) break; // 到后跳出
		}
		if (found) break;
	}

	if (found)
	{
		cv::Point a = cv::Point(target_centerx - width_findwindow / 2, target_centery - high_findwindow / 2);
		cv::Point b = cv::Point(target_centerx + width_findwindow / 2, target_centery + high_findwindow / 2);
		cv::rectangle(pt, a, b, cv::Scalar(0, 0, 255), 3, 4, 0);
	}

	//cv::imwrite("findpuzzle.jpg", pt);//保存搜索结果图片

	point_x = target_centerx;
	point_y = target_centery;
}
