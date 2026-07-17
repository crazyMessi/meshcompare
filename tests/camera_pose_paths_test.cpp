#include <QtTest>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "app/camera_pose_paths.h"

namespace
{
constexpr auto LibraryFileName = "meshlab_lizd_camera_poses.json";

QString legacyPath(const QString& root, const QString& application)
{
    return QDir(root).filePath(
        QStringLiteral("VCG/%1/%2")
            .arg(application, QString::fromLatin1(LibraryFileName)));
}

void createFile(const QString& path)
{
    QVERIFY(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    QCOMPARE(file.write("{}"), qint64(2));
}
} // namespace

class CameraPosePathsTest : public QObject
{
    Q_OBJECT

private slots:
    void newLibraryUsesTheApplicationLocalRootWithoutPrecisionSuffix()
    {
        const CameraPosePaths paths = resolveCameraPosePaths(
            QStringLiteral("/app-local/VCG/MeshCompare"),
            QStringLiteral("/generic-data"));

        QCOMPARE(
            paths.storagePath,
            QStringLiteral(
                "/app-local/VCG/MeshCompare/meshlab_lizd_camera_poses.json"));
        QVERIFY(!paths.storagePath.contains(QStringLiteral("64bit_fp")));
        QVERIFY(!paths.storagePath.contains(QStringLiteral("64bit_dp")));
    }

    void existingFloatLibraryWinsWhenBothLegacyPrecisionsExist()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString fp = legacyPath(root.path(), QStringLiteral("MeshLab_64bit_fp"));
        const QString dp = legacyPath(root.path(), QStringLiteral("MeshLab_64bit_dp"));
        createFile(dp);
        createFile(fp);

        const CameraPosePaths paths =
            resolveCameraPosePaths(QStringLiteral("/current"), root.path());

        QCOMPARE(paths.legacyPath, fp);
    }

    void doubleLibraryIsUsedOnlyWhenFloatLibraryIsMissing()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());
        const QString dp = legacyPath(root.path(), QStringLiteral("MeshLab_64bit_dp"));
        createFile(dp);

        const CameraPosePaths paths =
            resolveCameraPosePaths(QStringLiteral("/current"), root.path());

        QCOMPARE(paths.legacyPath, dp);
    }

    void missingLegacyFilesSelectTheFloatCandidateForANoOpMigration()
    {
        QTemporaryDir root;
        QVERIFY(root.isValid());

        const CameraPosePaths paths =
            resolveCameraPosePaths(QStringLiteral("/current"), root.path());

        QCOMPARE(
            paths.legacyPath,
            legacyPath(root.path(), QStringLiteral("MeshLab_64bit_fp")));
    }

    void emptyRootsStayEmptyInsteadOfProducingRelativePaths()
    {
        const CameraPosePaths paths = resolveCameraPosePaths({}, {});

        QVERIFY(paths.storagePath.isEmpty());
        QVERIFY(paths.legacyPath.isEmpty());
    }
};

QTEST_APPLESS_MAIN(CameraPosePathsTest)
#include "camera_pose_paths_test.moc"
