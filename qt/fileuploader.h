/*
 * Android File Transfer for Linux: MTP client for android devices
 * Copyright (C) 2015  Vladimir Menshakov

 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.

 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.

 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#ifndef FILEUPLOADER_H
#define FILEUPLOADER_H

#include <QObject>
#include <QThread>
#include <QDateTime>

class MtpObjectsModel;
struct Command;
class CommandQueue;

class FileUploader : public QObject
{
	Q_OBJECT

private:
	MtpObjectsModel	*	_model;
	QThread				_workerThread;
	CommandQueue *		_worker;
	qint64				_total;
	QDateTime			_startedAt;
	bool				_aborted;

private slots:
	void onProgress(qint64 current);
	void onStarted(const QString &file);
	void onFinished();

public:
	explicit FileUploader(MtpObjectsModel * model, QObject *parent = 0);
	~FileUploader();

	void upload(QStringList files);
	void download(const QString &path, const QVector<quint32> & objectIds);

public slots:
	void abort();

signals:
	void executeCommand(Command *cmd);

	//incoming signals (from worker)
	void uploadStarted(QString file);
	void uploadProgress(float);
	void uploadSpeed(qint64);
	void finished();
};

#endif // FILEUPLOADER_H
