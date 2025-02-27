/*
 * Copyright (C) by Klaas Freitag <freitag@owncloud.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "config.h"

#include "configfile.h"
#include "theme.h"
#include "version.h"
#include "common/utility.h"
#include "common/asserts.h"
#include "version.h"

#include "creds/abstractcredentials.h"
#include "creds/keychainchunk.h"

#include "csync_exclude.h"

#ifndef TOKEN_AUTH_ONLY
#include <QWidget>
#include <QHeaderView>
#endif

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QSettings>
#include <QNetworkProxy>
#include <QStandardPaths>

#define QTLEGACY (QT_VERSION < QT_VERSION_CHECK(5,9,0))

#if !(QTLEGACY)
#include <QOperatingSystemVersion>
#endif

#define DEFAULT_REMOTE_POLL_INTERVAL 30000 // default remote poll time in milliseconds
#define DEFAULT_MAX_LOG_LINES 20000

namespace {
static constexpr char showMainDialogAsNormalWindowC[] = "showMainDialogAsNormalWindow";
static constexpr char showConfigBackupWarningC[] = "showConfigBackupWarning";

static constexpr char remotePollIntervalC[] = "remotePollInterval";
static constexpr char forceSyncIntervalC[] = "forceSyncInterval";
static constexpr char fullLocalDiscoveryIntervalC[] = "fullLocalDiscoveryInterval";
static constexpr char notificationRefreshIntervalC[] = "notificationRefreshInterval";
static constexpr char monoIconsC[] = "monoIcons";
static constexpr char promptDeleteC[] = "promptDeleteAllFiles";
static constexpr char crashReporterC[] = "crashReporter";
static constexpr char optionalServerNotificationsC[] = "optionalServerNotifications";
static constexpr char showCallNotificationsC[] = "showCallNotifications";
static constexpr char showInExplorerNavigationPaneC[] = "showInExplorerNavigationPane";
static constexpr char skipUpdateCheckC[] = "skipUpdateCheck";
static constexpr char autoUpdateCheckC[] = "autoUpdateCheck";
static constexpr char updateCheckIntervalC[] = "updateCheckInterval";
static constexpr char updateSegmentC[] = "updateSegment";
static constexpr char updateChannelC[] = "updateChannel";
static constexpr char overrideServerUrlC[] = "overrideServerUrl";
static constexpr char overrideLocalDirC[] = "overrideLocalDir";
static constexpr char isVfsEnabledC[] = "isVfsEnabled";
static constexpr char geometryC[] = "geometry";
static constexpr char timeoutC[] = "timeout";
static constexpr char chunkSizeC[] = "chunkSize";
static constexpr char minChunkSizeC[] = "minChunkSize";
static constexpr char maxChunkSizeC[] = "maxChunkSize";
static constexpr char targetChunkUploadDurationC[] = "targetChunkUploadDuration";
static constexpr char automaticLogDirC[] = "logToTemporaryLogDir";
static constexpr char logDirC[] = "logDir";
static constexpr char logDebugC[] = "logDebug";
static constexpr char logExpireC[] = "logExpire";
static constexpr char logFlushC[] = "logFlush";
static constexpr char showExperimentalOptionsC[] = "showExperimentalOptions";
static constexpr char clientVersionC[] = "clientVersion";

static constexpr char proxyHostC[] = "Proxy/host";
static constexpr char proxyTypeC[] = "Proxy/type";
static constexpr char proxyPortC[] = "Proxy/port";
static constexpr char proxyUserC[] = "Proxy/user";
static constexpr char proxyPassC[] = "Proxy/pass";
static constexpr char proxyNeedsAuthC[] = "Proxy/needsAuth";

static constexpr char useUploadLimitC[] = "BWLimit/useUploadLimit";
static constexpr char useDownloadLimitC[] = "BWLimit/useDownloadLimit";
static constexpr char uploadLimitC[] = "BWLimit/uploadLimit";
static constexpr char downloadLimitC[] = "BWLimit/downloadLimit";

static constexpr char newBigFolderSizeLimitC[] = "newBigFolderSizeLimit";
static constexpr char useNewBigFolderSizeLimitC[] = "useNewBigFolderSizeLimit";
static constexpr char confirmExternalStorageC[] = "confirmExternalStorage";
static constexpr char moveToTrashC[] = "moveToTrash";

static constexpr char certPath[] = "http_certificatePath";
static constexpr char certPasswd[] = "http_certificatePasswd";

static const QSet validUpdateChannels { QStringLiteral("stable"), QStringLiteral("beta") };

static constexpr auto macFileProviderModuleEnabledC = "macFileProviderModuleEnabled";
}

namespace OCC {

namespace chrono = std::chrono;

Q_LOGGING_CATEGORY(lcConfigFile, "nextcloud.sync.configfile", QtInfoMsg)

QString ConfigFile::_confDir = {};
QString ConfigFile::_discoveredLegacyConfigPath = {};

static chrono::milliseconds millisecondsValue(const QSettings &setting, const char *key,
    chrono::milliseconds defaultValue)
{
    return chrono::milliseconds(setting.value(QLatin1String(key), qlonglong(defaultValue.count())).toLongLong());
}

bool copy_dir_recursive(QString from_dir, QString to_dir)
{
    QDir dir;
    dir.setPath(from_dir);

    from_dir += QDir::separator();
    to_dir += QDir::separator();

    foreach (QString copy_file, dir.entryList(QDir::Files)) {
        QString from = from_dir + copy_file;
        QString to = to_dir + copy_file;

        if (QFile::copy(from, to) == false) {
            return false;
        }
    }

    foreach (QString copy_dir, dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QString from = from_dir + copy_dir;
        QString to = to_dir + copy_dir;

        if (dir.mkpath(to) == false) {
            return false;
        }

        if (copy_dir_recursive(from, to) == false) {
            return false;
        }
    }

    return true;
}

ConfigFile::ConfigFile()
{
    // QDesktopServices uses the application name to create a config path
    qApp->setApplicationName(Theme::instance()->appNameGUI());

    QSettings::setDefaultFormat(QSettings::IniFormat);

    const QString config = configFile();


    QSettings settings(config, QSettings::IniFormat);
    settings.beginGroup(defaultConnection());
}

bool ConfigFile::setConfDir(const QString &value)
{
    QString dirPath = value;
    if (dirPath.isEmpty())
        return false;

    QFileInfo fi(dirPath);
    if (!fi.exists()) {
        QDir().mkpath(dirPath);
        fi.setFile(dirPath);
    }
    if (fi.exists() && fi.isDir()) {
        dirPath = fi.absoluteFilePath();
        qCInfo(lcConfigFile) << "Using custom config dir " << dirPath;
        _confDir = dirPath;
        return true;
    }
    return false;
}

bool ConfigFile::optionalServerNotifications() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(optionalServerNotificationsC), true).toBool();
}

bool ConfigFile::showCallNotifications() const
{
    const QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(showCallNotificationsC), true).toBool() && optionalServerNotifications();
}

void ConfigFile::setShowCallNotifications(bool show)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(showCallNotificationsC), show);
    settings.sync();
}

bool ConfigFile::showInExplorerNavigationPane() const
{
    const bool defaultValue =
#ifdef Q_OS_WIN
    #if QTLEGACY
        (QSysInfo::windowsVersion() < QSysInfo::WV_WINDOWS10);
    #else
        QOperatingSystemVersion::current() >= QOperatingSystemVersion::Windows10;
    #endif
#else
        false
#endif
        ;
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(showInExplorerNavigationPaneC), defaultValue).toBool();
}

void ConfigFile::setShowInExplorerNavigationPane(bool show)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(showInExplorerNavigationPaneC), show);
    settings.sync();
}

int ConfigFile::timeout() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(timeoutC), 300).toInt(); // default to 5 min
}

qint64 ConfigFile::chunkSize() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(chunkSizeC), 10 * 1000 * 1000).toLongLong(); // default to 10 MB
}

qint64 ConfigFile::maxChunkSize() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(maxChunkSizeC), 1000 * 1000 * 1000).toLongLong(); // default to 1000 MB
}

qint64 ConfigFile::minChunkSize() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(minChunkSizeC), 1000 * 1000).toLongLong(); // default to 1 MB
}

chrono::milliseconds ConfigFile::targetChunkUploadDuration() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return millisecondsValue(settings, targetChunkUploadDurationC, chrono::minutes(1));
}

void ConfigFile::setOptionalServerNotifications(bool show)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(optionalServerNotificationsC), show);
    settings.sync();
}

void ConfigFile::saveGeometry(QWidget *w)
{
#ifndef TOKEN_AUTH_ONLY
    ASSERT(!w->objectName().isNull());
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(w->objectName());
    settings.setValue(QLatin1String(geometryC), w->saveGeometry());
    settings.sync();
#endif
}

void ConfigFile::restoreGeometry(QWidget *w)
{
#ifndef TOKEN_AUTH_ONLY
    w->restoreGeometry(getValue(geometryC, w->objectName()).toByteArray());
#endif
}

void ConfigFile::saveGeometryHeader(QHeaderView *header)
{
#ifndef TOKEN_AUTH_ONLY
    if (!header)
        return;
    ASSERT(!header->objectName().isEmpty());

    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(header->objectName());
    settings.setValue(QLatin1String(geometryC), header->saveState());
    settings.sync();
#endif
}

void ConfigFile::restoreGeometryHeader(QHeaderView *header)
{
#ifndef TOKEN_AUTH_ONLY
    if (!header)
        return;
    ASSERT(!header->objectName().isNull());

    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(header->objectName());
    header->restoreState(settings.value(geometryC).toByteArray());
#endif
}

QVariant ConfigFile::getPolicySetting(const QString &setting, const QVariant &defaultValue) const
{
    if (Utility::isWindows()) {
        // check for policies first and return immediately if a value is found.
        QSettings userPolicy(QString::fromLatin1(R"(HKEY_CURRENT_USER\Software\Policies\%1\%2)")
                                 .arg(APPLICATION_VENDOR, Theme::instance()->appNameGUI()),
            QSettings::NativeFormat);
        if (userPolicy.contains(setting)) {
            return userPolicy.value(setting);
        }

        QSettings machinePolicy(QString::fromLatin1(R"(HKEY_LOCAL_MACHINE\Software\Policies\%1\%2)")
                                    .arg(APPLICATION_VENDOR, Theme::instance()->appNameGUI()),
            QSettings::NativeFormat);
        if (machinePolicy.contains(setting)) {
            return machinePolicy.value(setting);
        }
    }
    return defaultValue;
}

QString ConfigFile::configPath() const
{
    if (_confDir.isEmpty()) {
        if (!Utility::isWindows()) {
            // On Unix, use the AppConfigLocation for the settings, that's configurable with the XDG_CONFIG_HOME env variable.
            _confDir = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
        } else {
            // On Windows, use AppDataLocation, that's where the roaming data is and where we should store the config file
             auto newLocation = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

             // Check if this is the first time loading the new location
             if (!QFileInfo(newLocation).isDir()) {
                 // Migrate data to the new locations
                 auto oldLocation = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);

                 // Only migrate if the old location exists.
                 if (QFileInfo(oldLocation).isDir()) {
                     QDir().mkpath(newLocation);
                     copy_dir_recursive(oldLocation, newLocation);
                 }
             }
            _confDir = newLocation;
        }
    }
    QString dir = _confDir;

    if (!dir.endsWith(QLatin1Char('/')))
        dir.append(QLatin1Char('/'));
    return dir;
}

static const QLatin1String exclFile("sync-exclude.lst");

QString ConfigFile::excludeFile(Scope scope) const
{
    // prefer sync-exclude.lst, but if it does not exist, check for
    // exclude.lst for compatibility reasons in the user writeable
    // directories.
    QFileInfo fi;

    switch (scope) {
    case UserScope:
        fi.setFile(configPath(), exclFile);

        if (!fi.isReadable()) {
            fi.setFile(configPath(), QLatin1String("exclude.lst"));
        }
        if (!fi.isReadable()) {
            fi.setFile(configPath(), exclFile);
        }
        return fi.absoluteFilePath();
    case SystemScope:
        return ConfigFile::excludeFileFromSystem();
    }

    ASSERT(false);
    return QString();
}

QString ConfigFile::excludeFileFromSystem()
{
    QFileInfo fi;
#ifdef Q_OS_WIN
    fi.setFile(QCoreApplication::applicationDirPath(), exclFile);
#endif
#ifdef Q_OS_UNIX
    fi.setFile(QString(SYSCONFDIR "/" + Theme::instance()->appName()), exclFile);
    if (!fi.exists()) {
        // Prefer to return the preferred path! Only use the fallback location
        // if the other path does not exist and the fallback is valid.
        QFileInfo nextToBinary(QCoreApplication::applicationDirPath(), exclFile);
        if (nextToBinary.exists()) {
            fi = nextToBinary;
        } else {
            // For AppImage, the file might reside under a temporary mount path
            QDir d(QCoreApplication::applicationDirPath()); // supposed to be /tmp/mount.xyz/usr/bin
            d.cdUp(); // go out of bin
            d.cdUp(); // go out of usr
            if (!d.isRoot()) { // it is really a mountpoint
                if (d.cd("etc") && d.cd(Theme::instance()->appName())) {
                    QFileInfo inMountDir(d, exclFile);
                    if (inMountDir.exists()) {
                        fi = inMountDir;
                    }
                };
            }
        }
    }
#endif
#ifdef Q_OS_MAC
    // exec path is inside the bundle
    fi.setFile(QCoreApplication::applicationDirPath(),
        QLatin1String("../Resources/") + exclFile);
#endif

    return fi.absoluteFilePath();
}

QString ConfigFile::backup(const QString &fileName) const
{
    const QString baseFilePath = configPath() + fileName;
    auto versionString = clientVersionString();

    if (!versionString.isEmpty()) {
        versionString.prepend('_');
    }

    QString backupFile =
        QString("%1.backup_%2%3")
            .arg(baseFilePath)
            .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"))
            .arg(versionString);

    // If this exact file already exists it's most likely that a backup was
    // already done. (two backup calls directly after each other, potentially
    // even with source alterations in between!)
    // QFile does not overwrite backupFile
    if(!QFile::copy(baseFilePath, backupFile)) {
        qCWarning(lcConfigFile) << "Failed to create a backup of the config file" << baseFilePath;
    }

    return backupFile;
}

bool ConfigFile::showConfigBackupWarning() const
{
    return getValue(showConfigBackupWarningC, QString(), false).toBool();
}

QString ConfigFile::configFile() const
{
    return configPath() + Theme::instance()->configFileName();
}

bool ConfigFile::exists()
{
    QFile file(configFile());
    return file.exists();
}

QString ConfigFile::defaultConnection() const
{
    return Theme::instance()->appName();
}

void ConfigFile::storeData(const QString &group, const QString &key, const QVariant &value)
{
    const QString con(group.isEmpty() ? defaultConnection() : group);
    QSettings settings(configFile(), QSettings::IniFormat);

    settings.beginGroup(con);
    settings.setValue(key, value);
    settings.sync();
}

QVariant ConfigFile::retrieveData(const QString &group, const QString &key) const
{
    const QString con(group.isEmpty() ? defaultConnection() : group);
    QSettings settings(configFile(), QSettings::IniFormat);

    settings.beginGroup(con);
    return settings.value(key);
}

void ConfigFile::removeData(const QString &group, const QString &key)
{
    const QString con(group.isEmpty() ? defaultConnection() : group);
    QSettings settings(configFile(), QSettings::IniFormat);

    settings.beginGroup(con);
    settings.remove(key);
}

bool ConfigFile::dataExists(const QString &group, const QString &key) const
{
    const QString con(group.isEmpty() ? defaultConnection() : group);
    QSettings settings(configFile(), QSettings::IniFormat);

    settings.beginGroup(con);
    return settings.contains(key);
}

chrono::milliseconds ConfigFile::remotePollInterval(const QString &connection) const
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();

    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);

    auto defaultPollInterval = chrono::milliseconds(DEFAULT_REMOTE_POLL_INTERVAL);
    auto remoteInterval = millisecondsValue(settings, remotePollIntervalC, defaultPollInterval);
    if (remoteInterval < chrono::seconds(5)) {
        qCWarning(lcConfigFile) << "Remote Interval is less than 5 seconds, reverting to" << DEFAULT_REMOTE_POLL_INTERVAL;
        remoteInterval = defaultPollInterval;
    }
    return remoteInterval;
}

void ConfigFile::setRemotePollInterval(chrono::milliseconds interval, const QString &connection)
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();

    if (interval < chrono::seconds(5)) {
        qCWarning(lcConfigFile) << "Remote Poll interval of " << interval.count() << " is below five seconds.";
        return;
    }
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);
    settings.setValue(QLatin1String(remotePollIntervalC), qlonglong(interval.count()));
    settings.sync();
}

chrono::milliseconds ConfigFile::forceSyncInterval(const QString &connection) const
{
    auto pollInterval = remotePollInterval(connection);

    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);

    auto defaultInterval = chrono::hours(2);
    auto interval = millisecondsValue(settings, forceSyncIntervalC, defaultInterval);
    if (interval < pollInterval) {
        qCWarning(lcConfigFile) << "Force sync interval is less than the remote poll inteval, reverting to" << pollInterval.count();
        interval = pollInterval;
    }
    return interval;
}

chrono::milliseconds OCC::ConfigFile::fullLocalDiscoveryInterval() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(defaultConnection());
    return millisecondsValue(settings, fullLocalDiscoveryIntervalC, chrono::hours(1));
}

chrono::milliseconds ConfigFile::notificationRefreshInterval(const QString &connection) const
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);

    const auto defaultInterval = chrono::minutes(1);
    auto interval = millisecondsValue(settings, notificationRefreshIntervalC, defaultInterval);
    if (interval < chrono::minutes(1)) {
        qCWarning(lcConfigFile) << "Notification refresh interval smaller than one minute, setting to one minute";
        interval = chrono::minutes(1);
    }
    return interval;
}

chrono::milliseconds ConfigFile::updateCheckInterval(const QString &connection) const
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);

    auto defaultInterval = chrono::hours(10);
    auto interval = millisecondsValue(settings, updateCheckIntervalC, defaultInterval);

    auto minInterval = chrono::minutes(5);
    if (interval < minInterval) {
        qCWarning(lcConfigFile) << "Update check interval less than five minutes, resetting to 5 minutes";
        interval = minInterval;
    }
    return interval;
}

bool ConfigFile::skipUpdateCheck(const QString &connection) const
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();

    QVariant fallback = getValue(QLatin1String(skipUpdateCheckC), con, false);
    fallback = getValue(QLatin1String(skipUpdateCheckC), QString(), fallback);

    QVariant value = getPolicySetting(QLatin1String(skipUpdateCheckC), fallback);
    return value.toBool();
}

void ConfigFile::setSkipUpdateCheck(bool skip, const QString &connection)
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();

    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);

    settings.setValue(QLatin1String(skipUpdateCheckC), QVariant(skip));
    settings.sync();
}

bool ConfigFile::autoUpdateCheck(const QString &connection) const
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();

    QVariant fallback = getValue(QLatin1String(autoUpdateCheckC), con, true);
    fallback = getValue(QLatin1String(autoUpdateCheckC), QString(), fallback);

    QVariant value = getPolicySetting(QLatin1String(autoUpdateCheckC), fallback);
    return value.toBool();
}

void ConfigFile::setAutoUpdateCheck(bool autoCheck, const QString &connection)
{
    QString con(connection);
    if (connection.isEmpty())
        con = defaultConnection();

    QSettings settings(configFile(), QSettings::IniFormat);
    settings.beginGroup(con);

    settings.setValue(QLatin1String(autoUpdateCheckC), QVariant(autoCheck));
    settings.sync();
}

int ConfigFile::updateSegment() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    int segment = settings.value(QLatin1String(updateSegmentC), -1).toInt();

    // Invalid? (Unset at the very first launch)
    if(segment < 0 || segment > 99) {
        // Save valid segment value, normally has to be done only once.
        segment = Utility::rand() % 99;
        settings.setValue(QLatin1String(updateSegmentC), segment);
    }

    return segment;
}

QString ConfigFile::updateChannel() const
{
    QString defaultUpdateChannel = QStringLiteral("stable");
    QString suffix = QString::fromLatin1(MIRALL_STRINGIFY(MIRALL_VERSION_SUFFIX));
    if (suffix.startsWith("daily")
        || suffix.startsWith("nightly")
        || suffix.startsWith("alpha")
        || suffix.startsWith("rc")
        || suffix.startsWith("beta")) {
        defaultUpdateChannel = QStringLiteral("beta");
    }

    QSettings settings(configFile(), QSettings::IniFormat);
    const auto channel = settings.value(QLatin1String(updateChannelC), defaultUpdateChannel).toString();
    if (!validUpdateChannels.contains(channel)) {
        qCWarning(lcConfigFile()) << "Received invalid update channel from confog:"
                                  << channel
                                  << "defaulting to:"
                                  << defaultUpdateChannel;
        return defaultUpdateChannel;
    }

    return channel;
}

void ConfigFile::setUpdateChannel(const QString &channel)
{
    if (!validUpdateChannels.contains(channel)) {
        qCWarning(lcConfigFile()) << "Received invalid update channel:"
                                  << channel
                                  << "can only accept 'stable' or 'beta'. Ignoring.";
        return;
    }

    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(updateChannelC), channel);
}

[[nodiscard]] QString ConfigFile::overrideServerUrl() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(overrideServerUrlC), {}).toString();
}

void ConfigFile::setOverrideServerUrl(const QString &url)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(overrideServerUrlC), url);
}

[[nodiscard]] QString ConfigFile::overrideLocalDir() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(overrideLocalDirC), {}).toString();
}

void ConfigFile::setOverrideLocalDir(const QString &localDir)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(overrideLocalDirC), localDir);
}

bool ConfigFile::isVfsEnabled() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value({isVfsEnabledC}, {}).toBool();
}

void ConfigFile::setVfsEnabled(bool enabled)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue({isVfsEnabledC}, enabled);
}

void ConfigFile::setProxyType(int proxyType,
    const QString &host,
    int port, bool needsAuth,
    const QString &user,
    const QString &pass)
{
    QSettings settings(configFile(), QSettings::IniFormat);

    settings.setValue(QLatin1String(proxyTypeC), proxyType);

    if (proxyType == QNetworkProxy::HttpProxy || proxyType == QNetworkProxy::Socks5Proxy) {
        settings.setValue(QLatin1String(proxyHostC), host);
        settings.setValue(QLatin1String(proxyPortC), port);
        settings.setValue(QLatin1String(proxyNeedsAuthC), needsAuth);
        settings.setValue(QLatin1String(proxyUserC), user);

        if (pass.isEmpty()) {
            // Security: Don't keep password in config file
            settings.remove(QLatin1String(proxyPassC));

            // Delete password from keychain
            auto job = new KeychainChunk::DeleteJob(keychainProxyPasswordKey());
            job->exec();
        } else {
            // Write password to keychain
            auto job = new KeychainChunk::WriteJob(keychainProxyPasswordKey(), pass.toUtf8());
            if (job->exec()) {
                // Security: Don't keep password in config file
                settings.remove(QLatin1String(proxyPassC));
            }
        }
    }
    settings.sync();
}

QVariant ConfigFile::getValue(const QString &param, const QString &group,
    const QVariant &defaultValue) const
{
    QVariant systemSetting;
    if (Utility::isMac()) {
        QSettings systemSettings(QLatin1String("/Library/Preferences/" APPLICATION_REV_DOMAIN ".plist"), QSettings::NativeFormat);
        if (!group.isEmpty()) {
            systemSettings.beginGroup(group);
        }
        systemSetting = systemSettings.value(param, defaultValue);
    } else if (Utility::isUnix()) {
        QSettings systemSettings(QString(SYSCONFDIR "/%1/%1.conf").arg(Theme::instance()->appName()), QSettings::NativeFormat);
        if (!group.isEmpty()) {
            systemSettings.beginGroup(group);
        }
        systemSetting = systemSettings.value(param, defaultValue);
    } else { // Windows
        QSettings systemSettings(QString::fromLatin1(R"(HKEY_LOCAL_MACHINE\Software\%1\%2)")
                                     .arg(APPLICATION_VENDOR, Theme::instance()->appNameGUI()),
            QSettings::NativeFormat);
        if (!group.isEmpty()) {
            systemSettings.beginGroup(group);
        }
        systemSetting = systemSettings.value(param, defaultValue);
    }

    QSettings settings(configFile(), QSettings::IniFormat);
    if (!group.isEmpty())
        settings.beginGroup(group);

    return settings.value(param, systemSetting);
}

void ConfigFile::setValue(const QString &key, const QVariant &value)
{
    QSettings settings(configFile(), QSettings::IniFormat);

    settings.setValue(key, value);
}

int ConfigFile::proxyType() const
{
    if (Theme::instance()->forceSystemNetworkProxy()) {
        return QNetworkProxy::DefaultProxy;
    }
    return getValue(QLatin1String(proxyTypeC)).toInt();
}

QString ConfigFile::proxyHostName() const
{
    return getValue(QLatin1String(proxyHostC)).toString();
}

int ConfigFile::proxyPort() const
{
    return getValue(QLatin1String(proxyPortC)).toInt();
}

bool ConfigFile::proxyNeedsAuth() const
{
    return getValue(QLatin1String(proxyNeedsAuthC)).toBool();
}

QString ConfigFile::proxyUser() const
{
    return getValue(QLatin1String(proxyUserC)).toString();
}

QString ConfigFile::proxyPassword() const
{
    QByteArray passEncoded = getValue(proxyPassC).toByteArray();
    auto pass = QString::fromUtf8(QByteArray::fromBase64(passEncoded));
    passEncoded.clear();

    const auto key = keychainProxyPasswordKey();

    if (!pass.isEmpty()) {
        // Security: Migrate password from config file to keychain
        auto job = new KeychainChunk::WriteJob(key, pass.toUtf8());
        if (job->exec()) {
            QSettings settings(configFile(), QSettings::IniFormat);
            settings.remove(QLatin1String(proxyPassC));
            qCInfo(lcConfigFile()) << "Migrated proxy password to keychain";
        }
    } else {
        // Read password from keychain
        auto job = new KeychainChunk::ReadJob(key);
        if (job->exec()) {
            pass = job->textData();
        }
    }

    return pass;
}

QString ConfigFile::keychainProxyPasswordKey() const
{
    return QString::fromLatin1("proxy-password");
}

int ConfigFile::useUploadLimit() const
{
    return getValue(useUploadLimitC, QString(), 0).toInt();
}

int ConfigFile::useDownloadLimit() const
{
    return getValue(useDownloadLimitC, QString(), 0).toInt();
}

void ConfigFile::setUseUploadLimit(int val)
{
    setValue(useUploadLimitC, val);
}

void ConfigFile::setUseDownloadLimit(int val)
{
    setValue(useDownloadLimitC, val);
}

int ConfigFile::uploadLimit() const
{
    return getValue(uploadLimitC, QString(), 10).toInt();
}

int ConfigFile::downloadLimit() const
{
    return getValue(downloadLimitC, QString(), 80).toInt();
}

void ConfigFile::setUploadLimit(int kbytes)
{
    setValue(uploadLimitC, kbytes);
}

void ConfigFile::setDownloadLimit(int kbytes)
{
    setValue(downloadLimitC, kbytes);
}

QPair<bool, qint64> ConfigFile::newBigFolderSizeLimit() const
{
    auto defaultValue = Theme::instance()->newBigFolderSizeLimit();
    const auto fallback = getValue(newBigFolderSizeLimitC, QString(), defaultValue).toLongLong();
    const auto value = getPolicySetting(QLatin1String(newBigFolderSizeLimitC), fallback).toLongLong();
    const bool use = value >= 0 && useNewBigFolderSizeLimit();
    return qMakePair(use, qMax<qint64>(0, value));
}

void ConfigFile::setNewBigFolderSizeLimit(bool isChecked, qint64 mbytes)
{
    setValue(newBigFolderSizeLimitC, mbytes);
    setValue(useNewBigFolderSizeLimitC, isChecked);
}

bool ConfigFile::confirmExternalStorage() const
{
    const auto fallback = getValue(confirmExternalStorageC, QString(), true);
    return getPolicySetting(QLatin1String(confirmExternalStorageC), fallback).toBool();
}

bool ConfigFile::useNewBigFolderSizeLimit() const
{
    const auto fallback = getValue(useNewBigFolderSizeLimitC, QString(), true);
    return getPolicySetting(QLatin1String(useNewBigFolderSizeLimitC), fallback).toBool();
}

void ConfigFile::setConfirmExternalStorage(bool isChecked)
{
    setValue(confirmExternalStorageC, isChecked);
}

bool ConfigFile::moveToTrash() const
{
    return getValue(moveToTrashC, QString(), false).toBool();
}

void ConfigFile::setMoveToTrash(bool isChecked)
{
    setValue(moveToTrashC, isChecked);
}

bool ConfigFile::showMainDialogAsNormalWindow() const {
    return getValue(showMainDialogAsNormalWindowC, {}, false).toBool();
}

bool ConfigFile::promptDeleteFiles() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(promptDeleteC), false).toBool();
}

void ConfigFile::setPromptDeleteFiles(bool promptDeleteFiles)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(promptDeleteC), promptDeleteFiles);
}

bool ConfigFile::monoIcons() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    bool monoDefault = false; // On Mac we want bw by default
#ifdef Q_OS_MAC
    // OEM themes are not obliged to ship mono icons
    monoDefault = QByteArrayLiteral("Nextcloud") == QByteArrayLiteral(APPLICATION_NAME);
#endif
    return settings.value(QLatin1String(monoIconsC), monoDefault).toBool();
}

void ConfigFile::setMonoIcons(bool useMonoIcons)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(monoIconsC), useMonoIcons);
}

bool ConfigFile::crashReporter() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    const auto fallback = settings.value(QLatin1String(crashReporterC), true);
    return getPolicySetting(QLatin1String(crashReporterC), fallback).toBool();
}

void ConfigFile::setCrashReporter(bool enabled)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(crashReporterC), enabled);
}

bool ConfigFile::automaticLogDir() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(automaticLogDirC), false).toBool();
}

void ConfigFile::setAutomaticLogDir(bool enabled)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(automaticLogDirC), enabled);
}

QString ConfigFile::logDir() const
{
    const auto defaultLogDir = QString(configPath() + QStringLiteral("/logs"));
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(logDirC), defaultLogDir).toString();
}

void ConfigFile::setLogDir(const QString &dir)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(logDirC), dir);
}

bool ConfigFile::logDebug() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(logDebugC), false).toBool();
}

void ConfigFile::setLogDebug(bool enabled)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(logDebugC), enabled);
}

int ConfigFile::logExpire() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(logExpireC), 24).toInt();
}

void ConfigFile::setLogExpire(int hours)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(logExpireC), hours);
}

bool ConfigFile::logFlush() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(logFlushC), false).toBool();
}

void ConfigFile::setLogFlush(bool enabled)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(logFlushC), enabled);
}

bool ConfigFile::showExperimentalOptions() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(showExperimentalOptionsC), false).toBool();
}

QString ConfigFile::certificatePath() const
{
    return retrieveData(QString(), QLatin1String(certPath)).toString();
}

void ConfigFile::setCertificatePath(const QString &cPath)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(certPath), cPath);
    settings.sync();
}

QString ConfigFile::certificatePasswd() const
{
    return retrieveData(QString(), QLatin1String(certPasswd)).toString();
}

void ConfigFile::setCertificatePasswd(const QString &cPasswd)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(certPasswd), cPasswd);
    settings.sync();
}

QString ConfigFile::clientVersionString() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(QLatin1String(clientVersionC), QString()).toString();
}

void ConfigFile::setClientVersionString(const QString &version)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(clientVersionC), version);
}

Q_GLOBAL_STATIC(QString, g_configFileName)

std::unique_ptr<QSettings> ConfigFile::settingsWithGroup(const QString &group, QObject *parent)
{
    if (g_configFileName()->isEmpty()) {
        // cache file name
        ConfigFile cfg;
        *g_configFileName() = cfg.configFile();
    }
    std::unique_ptr<QSettings> settings(new QSettings(*g_configFileName(), QSettings::IniFormat, parent));
    settings->beginGroup(group);
    return settings;
}

void ConfigFile::setupDefaultExcludeFilePaths(ExcludedFiles &excludedFiles)
{
    ConfigFile cfg;
    QString systemList = cfg.excludeFile(ConfigFile::SystemScope);
    QString userList = cfg.excludeFile(ConfigFile::UserScope);

    if (!QFile::exists(userList)) {
        qCInfo(lcConfigFile) << "User defined ignore list does not exist:" << userList;
        if (!QFile::copy(systemList, userList)) {
            qCInfo(lcConfigFile) << "Could not copy over default list to:" << userList;
        }
    }

    if (!QFile::exists(userList)) {
        qCInfo(lcConfigFile) << "Adding system ignore list to csync:" << systemList;
        excludedFiles.addExcludeFilePath(systemList);
    } else {
        qCInfo(lcConfigFile) << "Adding user defined ignore list to csync:" << userList;
        excludedFiles.addExcludeFilePath(userList);
    }
}

QString ConfigFile::discoveredLegacyConfigPath()
{
    return _discoveredLegacyConfigPath;
}

void ConfigFile::setDiscoveredLegacyConfigPath(const QString &discoveredLegacyConfigPath)
{
    if (_discoveredLegacyConfigPath == discoveredLegacyConfigPath) {
        return;
    }

    _discoveredLegacyConfigPath = discoveredLegacyConfigPath;
}

bool ConfigFile::macFileProviderModuleEnabled() const
{
    QSettings settings(configFile(), QSettings::IniFormat);
    return settings.value(macFileProviderModuleEnabledC, false).toBool();
}

void ConfigFile::setMacFileProviderModuleEnabled(const bool moduleEnabled)
{
    QSettings settings(configFile(), QSettings::IniFormat);
    settings.setValue(QLatin1String(macFileProviderModuleEnabledC), moduleEnabled);
}

}
