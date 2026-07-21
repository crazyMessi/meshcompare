#include <QtTest>

#include "app/startup_command_line.h"

class StartupCommandLineTest : public QObject
{
    Q_OBJECT

private slots:
    void gridOptionStartsOneProjectInComparisonGrid()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--grid"),
             QStringLiteral("comparison.MLP")});

        QVERIFY(command.ok);
        QVERIFY(command.startInComparisonGrid);
        QCOMPARE(command.inputPaths, QStringList{QStringLiteral("comparison.MLP")});
    }

    void normalMeshArgumentsRemainUnchanged()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("reference_gt.obj"),
             QStringLiteral("candidate.obj")});

        QVERIFY(command.ok);
        QVERIFY(!command.startInComparisonGrid);
        const QStringList expectedPaths{
            QStringLiteral("reference_gt.obj"),
            QStringLiteral("candidate.obj")};
        QCOMPARE(command.inputPaths, expectedPaths);
    }

    void gridOptionRejectsAnythingOtherThanOneProject()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"),
             QStringLiteral("--grid"),
             QStringLiteral("reference.obj"),
             QStringLiteral("candidate.obj")});

        QVERIFY(!command.ok);
        QCOMPARE(
            command.error,
            QStringLiteral("--grid requires exactly one MeshLab project (*.mlp)."));
    }

    void helpOptionDoesNotStartTheApplication()
    {
        const StartupCommandLine command = parseStartupCommandLine(
            {QStringLiteral("meshcompare"), QStringLiteral("--help")});

        QVERIFY(command.ok);
        QVERIFY(command.showHelp);
        QVERIFY(command.inputPaths.isEmpty());
        QVERIFY(startupCommandLineUsage().contains(
            QStringLiteral("meshcompare --grid comparison.mlp")));
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
