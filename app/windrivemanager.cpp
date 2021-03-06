/*
 * Fedora Media Writer
 * Copyright (C) 2016 Martin Bříza <mbriza@redhat.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#include "windrivemanager.h"

#include <QTimer>
#include <QDebug>

#include <windows.h>

WinDriveProvider::WinDriveProvider(DriveManager *parent)
    : DriveProvider(parent)
{
    QTimer::singleShot(0, this, &WinDriveProvider::checkDrives);
}

void WinDriveProvider::checkDrives() {
    QSet<int> drivesWithLetters;

    DWORD drives = ::GetLogicalDrives();
    for (char i = 0; i < 26; i++) {
        if (drives & (1 << i)) {
            drivesWithLetters.unite(findPhysicalDrive('A' + i));
        }
    }

    for (int i = 0; i < 64; i++) {
        bool present = describeDrive(i, drivesWithLetters.contains(i));
        if (!present && m_drives.contains(i)) {
            emit driveRemoved(m_drives[i]);
            m_drives[i]->deleteLater();
            m_drives.remove(i);
        }
    }

    QTimer::singleShot(2500, this, &WinDriveProvider::checkDrives);
}

QSet<int> WinDriveProvider::findPhysicalDrive(char driveLetter) {
    QSet<int> ret;

    QString drivePath = QString("\\\\.\\%1:").arg(driveLetter);

    HANDLE hDevice = ::CreateFile(drivePath.toStdWString().c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    DWORD bytesReturned;
    VOLUME_DISK_EXTENTS vde; // TODO FIXME: handle ERROR_MORE_DATA (this is an extending structure)
    BOOL bResult = DeviceIoControl(hDevice, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, NULL, 0, &vde, sizeof(vde), &bytesReturned, NULL);

    if (!bResult) {
        qCritical() << "Could not get disk extents";
    }

    for (uint i = 0; i < vde.NumberOfDiskExtents; i++) {
        /*
         * FIXME?
         * This is a bit more complicated matter.
         * Windows doesn't seem to provide the complete information about the drive (not just in this API).
         * That's the reason I chose to detect it by looking at the partition's size.
         * An even better approach would be to compare it to the size of the drive itself but for now this will have to suffice.
         */
        if (vde.Extents[i].ExtentLength.QuadPart > 100 * 1024 * 1024) // only partitions bigger than 100MB
            ret.insert(vde.Extents[i].DiskNumber);
    }

    return ret;
}

bool WinDriveProvider::describeDrive(int nDriveNumber, bool hasLetter) {
    BOOL removable;
    QString productVendor;
    QString productId;
    QString serialNumber;
    uint64_t deviceBytes;


    BOOL bResult   = FALSE;                 // results flag
    //DWORD dwRet = NO_ERROR;

    // Format physical drive path (may be '\\.\PhysicalDrive0', '\\.\PhysicalDrive1' and so on).
    QString strDrivePath = QString("\\\\.\\PhysicalDrive%0").arg(nDriveNumber);

    // Get a handle to physical drive
    HANDLE hDevice = ::CreateFile(strDrivePath.toStdWString().c_str(), 0, FILE_SHARE_READ|FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, 0, NULL);

    if(INVALID_HANDLE_VALUE == hDevice)
        return false; //::GetLastError();

    // Set the input data structure
    STORAGE_PROPERTY_QUERY storagePropertyQuery;
    ZeroMemory(&storagePropertyQuery, sizeof(STORAGE_PROPERTY_QUERY));
    storagePropertyQuery.PropertyId = StorageDeviceProperty;
    storagePropertyQuery.QueryType = PropertyStandardQuery;

    // Get the necessary output buffer size
    STORAGE_DESCRIPTOR_HEADER storageDescriptorHeader;
    ZeroMemory(&storageDescriptorHeader, sizeof(STORAGE_DESCRIPTOR_HEADER));
    DWORD dwBytesReturned = 0;
    if(! ::DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
        &storagePropertyQuery, sizeof(STORAGE_PROPERTY_QUERY),
        &storageDescriptorHeader, sizeof(STORAGE_DESCRIPTOR_HEADER),
        &dwBytesReturned, NULL))
    {
        //dwRet = ::GetLastError();
        ::CloseHandle(hDevice);
        return false; // dwRet;
    }

    // Alloc the output buffer
    const DWORD dwOutBufferSize = storageDescriptorHeader.Size;
    BYTE* pOutBuffer = new BYTE[dwOutBufferSize];
    ZeroMemory(pOutBuffer, dwOutBufferSize);

    // Get the storage device descriptor
    if(!(bResult = ::DeviceIoControl(hDevice, IOCTL_STORAGE_QUERY_PROPERTY,
            &storagePropertyQuery, sizeof(STORAGE_PROPERTY_QUERY),
            pOutBuffer, dwOutBufferSize,
            &dwBytesReturned, NULL)))
    {
        //dwRet = ::GetLastError();
        delete []pOutBuffer;
        ::CloseHandle(hDevice);
        return false; // dwRet;
    }

    // Now, the output buffer points to a STORAGE_DEVICE_DESCRIPTOR structure
    // followed by additional info like vendor ID, product ID, serial number, and so on.
    STORAGE_DEVICE_DESCRIPTOR* pDeviceDescriptor = (STORAGE_DEVICE_DESCRIPTOR*)pOutBuffer;
    removable = pDeviceDescriptor->RemovableMedia;
    if (pDeviceDescriptor->ProductIdOffset != 0)
        productId = QString((char*) pOutBuffer + pDeviceDescriptor->ProductIdOffset).trimmed();
    if (pDeviceDescriptor->VendorIdOffset != 0)
        productVendor = QString((char*) pOutBuffer + pDeviceDescriptor->VendorIdOffset).trimmed();
    if (pDeviceDescriptor->SerialNumberOffset != 0)
        serialNumber = QString((char*) pOutBuffer + pDeviceDescriptor->SerialNumberOffset).trimmed();

    if (!removable)
        return false;

    DISK_GEOMETRY pdg;
    DWORD junk     = 0;                     // discard results

    bResult = DeviceIoControl(hDevice,                       // device to be queried
                              IOCTL_DISK_GET_DRIVE_GEOMETRY, // operation to perform
                              NULL, 0,                       // no input buffer
                              &pdg, sizeof(pdg),            // output buffer
                              &junk,                         // # bytes returned
                              (LPOVERLAPPED) NULL);          // synchronous I/O

    deviceBytes = pdg.Cylinders.QuadPart * pdg.TracksPerCylinder * pdg.SectorsPerTrack * pdg.BytesPerSector;

    // Do cleanup and return
    delete []pOutBuffer;
    ::CloseHandle(hDevice);

    WinDrive *currentDrive = new WinDrive(this, productVendor + " " + productId, deviceBytes, !hasLetter, nDriveNumber, serialNumber);
    if (m_drives.contains(nDriveNumber) && *m_drives[nDriveNumber] == *currentDrive) {
        currentDrive->deleteLater();
        return true;
    }

    if (m_drives.contains(nDriveNumber)) {
        emit driveRemoved(m_drives[nDriveNumber]);
        m_drives[nDriveNumber]->deleteLater();
    }

    m_drives[nDriveNumber] = currentDrive;
    emit driveConnected(currentDrive);

    return true;
}

WinDrive::WinDrive(WinDriveProvider *parent, const QString &name, uint64_t size, bool containsLive, int device, const QString &serialNumber)
    : Drive(parent, name, size, containsLive)
    , m_device(device)
    , m_serialNo(serialNumber)
{

}

WinDrive::~WinDrive() {
    if (m_child)
        m_child->kill();
}

void WinDrive::write(ReleaseVariant *data) {
    Drive::write(data);

    if (m_child) {
        // TODO some handling of an already present process
        m_child->deleteLater();
    }
    m_child = new QProcess(this);
    connect(m_child, static_cast<void(QProcess::*)(int, QProcess::ExitStatus)>(&QProcess::finished), this, &WinDrive::onFinished);
    connect(m_child, &QProcess::readyRead, this, &WinDrive::onReadyRead);

    if (QFile::exists(qApp->applicationDirPath() + "/helper.exe")) {
        m_child->setProgram(qApp->applicationDirPath() + "/helper.exe");
    }
    else if (QFile::exists(qApp->applicationDirPath() + "/../helper.exe")) {
        m_child->setProgram(qApp->applicationDirPath() + "/../helper.exe");
    }
    else {
        data->setErrorString("Your installation is broken. Couldn't find the helper program.");
        return;
    }

    QStringList args;
    args << "write";
    args << data->iso();
    args << QString("%1").arg(m_device);
    m_child->setArguments(args);
    m_child->setProcessChannelMode(QProcess::ForwardedErrorChannel);

    m_progress->setTo(data->size());
    m_image->setStatus(ReleaseVariant::WRITING);

    m_child->start();
}

void WinDrive::restore() {
    qCritical() << "starting to restore";
    if (m_child)
        m_child->deleteLater();

    m_child = new QProcess(this);

    m_restoreStatus = RESTORING;
    emit restoreStatusChanged();

    if (QFile::exists(qApp->applicationDirPath() + "/helper.exe")) {
        m_child->setProgram(qApp->applicationDirPath() + "/helper.exe");
    }
    else if (QFile::exists(qApp->applicationDirPath() + "/../helper.exe")) {
        m_child->setProgram(qApp->applicationDirPath() + "/../helper.exe");
    }
    else {
        m_restoreStatus = RESTORE_ERROR;
        return;
    }

    QStringList args;
    args << "restore";
    args << QString("%1").arg(m_device);
    m_child->setArguments(args);

    //connect(m_process, &QProcess::readyRead, this, &LinuxDrive::onReadyRead);
    connect(m_child, SIGNAL(finished(int,QProcess::ExitStatus)), this, SLOT(onRestoreFinished(int,QProcess::ExitStatus)));

    m_child->start(QIODevice::ReadOnly);
}

QString WinDrive::serialNumber() const {
    return m_serialNo;
}

bool WinDrive::operator==(const WinDrive &o) const {
    return (o.serialNumber() == serialNumber()) && Drive::operator==(o);
}

void WinDrive::onFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    qDebug() << "Child finished" << exitCode << m_child->errorString() << exitStatus;

    if (exitCode == 0) {
        m_image->setStatus(ReleaseVariant::FINISHED);
    }
    else {
        m_image->setErrorString(m_child->readAllStandardError());
        m_image->setStatus(ReleaseVariant::FAILED);
    }

    m_child->deleteLater();
    m_child = nullptr;
}

void WinDrive::onRestoreFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    qCritical() << "Process finished" << exitCode << exitStatus;
    qCritical() << m_child->readAllStandardError();

    if (exitCode == 0)
        m_restoreStatus = RESTORED;
    else
        m_restoreStatus = RESTORE_ERROR;
    emit restoreStatusChanged();

    m_child->deleteLater();
    m_child = nullptr;
}

void WinDrive::onReadyRead() {
    while (m_child->bytesAvailable() > 0) {
        bool ok;
        int64_t bytes = m_child->readLine().trimmed().toULongLong(&ok);
        if (ok)
            m_progress->setValue(bytes);
    }
}
