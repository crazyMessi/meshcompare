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
             QStringLiteral("/tmp/rendered")});

        QVERIFY(command.ok);
        QVERIFY(command.renderComparisonGrid);
        QCOMPARE(command.inputPaths, QStringList{QStringLiteral("comparison.MLP")});
        QCOMPARE(command.outputDirectory, QStringLiteral("/tmp/rendered"));
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

    void helpOptionDoesNotStartTheApplication()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"), QStringLiteral("--help")});

        QVERIFY(command.ok);
        QVERIFY(command.showHelp);
        QVERIFY(command.inputPaths.isEmpty());
        QVERIFY(startupCommandLineUsage().contains(
            QStringLiteral("meshcompare --render-grid comparison.mlp --output-dir output")));
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
