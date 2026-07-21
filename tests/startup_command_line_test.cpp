#include <QtTest>

#include "app/startup_command_line.h"

class StartupCommandLineTest : public QObject
{
    Q_OBJECT

private slots:
    void renderGridOptionCapturesOneProjectAndAnOutputDirectory()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("comparison.MLP"),
             QStringLiteral("--output-dir"),
             QStringLiteral("/tmp/rendered"),
             QStringLiteral("--size"),
             QStringLiteral("3840x2160")});

        QVERIFY(command.ok);
        QVERIFY(command.renderComparisonGrid);
        QCOMPARE(command.inputPaths, QStringList{QStringLiteral("comparison.MLP")});
        QCOMPARE(command.outputDirectory, QStringLiteral("/tmp/rendered"));
        QCOMPARE(command.outputSize, QSize(3840, 2160));
    }

    void normalMeshArgumentsRemainUnchanged()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("reference_gt.obj"),
             QStringLiteral("candidate.obj")});

        QVERIFY(command.ok);
        QVERIFY(!command.renderComparisonGrid);
        const QStringList expectedPaths{
            QStringLiteral("reference_gt.obj"),
            QStringLiteral("candidate.obj")};
        QCOMPARE(command.inputPaths, expectedPaths);
    }

    void renderGridRequiresOneProjectAndAnOutputDirectory()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("reference.obj"),
             QStringLiteral("candidate.obj")});

        QVERIFY(!command.ok);
        QCOMPARE(
            command.error,
            QStringLiteral(
                "--render-grid requires exactly one MeshLab project (*.mlp) and --output-dir."));
    }

    void renderGridRejectsInvalidImageSize()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("comparison.mlp"),
             QStringLiteral("--output-dir"),
             QStringLiteral("/tmp/rendered"),
             QStringLiteral("--size"),
             QStringLiteral("3840-by-2160")});

        QVERIFY(!command.ok);
        QCOMPARE(
            command.error,
            QStringLiteral(
                "--size must use positive WIDTHxHEIGHT dimensions, at most 16384 per side and 67108864 pixels."));
    }

    void renderGridRejectsUnreasonablyLargeImageSize()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("comparison.mlp"),
             QStringLiteral("--output-dir"),
             QStringLiteral("/tmp/rendered"),
             QStringLiteral("--size"),
             QStringLiteral("16384x16384")});

        QVERIFY(!command.ok);
        QCOMPARE(
            command.error,
            QStringLiteral(
                "--size must use positive WIDTHxHEIGHT dimensions, at most 16384 per side and 67108864 pixels."));
    }

    void renderGridAcceptsCameraPositionAndTarget()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("comparison.mlp"),
             QStringLiteral("--output-dir"),
             QStringLiteral("/tmp/rendered"),
             QStringLiteral("--camera"),
             QStringLiteral("1.5,2,3"),
             QStringLiteral("--look-at"),
             QStringLiteral("0,0,0"),
             QStringLiteral("--up"),
             QStringLiteral("0,0,1"),
             QStringLiteral("--fov"),
             QStringLiteral("45")});

        QVERIFY(command.ok);
        QVERIFY(command.camera.enabled);
        QCOMPARE(command.camera.position.x, 1.5);
        QCOMPARE(command.camera.position.y, 2.0);
        QCOMPARE(command.camera.position.z, 3.0);
        QCOMPARE(command.camera.target.x, 0.0);
        QCOMPARE(command.camera.up.z, 1.0);
        QCOMPARE(command.camera.fieldOfViewDegrees, 45.0);
    }

    void renderGridRequiresCameraAndTargetTogether()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("comparison.mlp"),
             QStringLiteral("--output-dir"),
             QStringLiteral("/tmp/rendered"),
             QStringLiteral("--camera"),
             QStringLiteral("1,2,3")});

        QVERIFY(!command.ok);
        QCOMPARE(
            command.error,
            QStringLiteral("--camera and --look-at must be provided together."));
    }

    void renderGridReportsAnInvalidFieldOfView()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--render-grid"),
             QStringLiteral("comparison.mlp"),
             QStringLiteral("--output-dir"),
             QStringLiteral("/tmp/rendered"),
             QStringLiteral("--camera"),
             QStringLiteral("1,2,3"),
             QStringLiteral("--look-at"),
             QStringLiteral("0,0,0"),
             QStringLiteral("--fov"),
             QStringLiteral("not-a-number")});

        QVERIFY(!command.ok);
        QCOMPARE(
            command.error,
            QStringLiteral("--fov requires a finite number of degrees."));
    }

    void helpOptionDoesNotStartTheApplication()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"), QStringLiteral("--help")});

        QVERIFY(command.ok);
        QVERIFY(command.showHelp);
        QVERIFY(command.inputPaths.isEmpty());
        QVERIFY(startupCommandLineUsage().contains(
            QStringLiteral(
                "meshcompare --render-grid comparison.mlp --output-dir output [--size WIDTHxHEIGHT]")));
    }

    void pathsThatStartWithDashesRemainInputs()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("-reference.obj"),
             QStringLiteral("candidate.obj")});

        QVERIFY(command.ok);
        const QStringList expectedPaths{
            QStringLiteral("-reference.obj"),
            QStringLiteral("candidate.obj")};
        QCOMPARE(command.inputPaths, expectedPaths);
    }
};

QTEST_APPLESS_MAIN(StartupCommandLineTest)
#include "startup_command_line_test.moc"
