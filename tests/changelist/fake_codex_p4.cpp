// Offline fake CLI used by changelist-tests to simulate both p4.exe and codex.exe.
#include <QCoreApplication>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>
#include <QThread>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    const QString stateFile = qEnvironmentVariable("P4VPATCH_TEST_STATE");
    QSettings state(stateFile, QSettings::IniFormat);
    const QString scenario = state.value(QStringLiteral("scenario")).toString();

    if (args.contains(QStringLiteral("app-server"))) {
        state.setValue(QStringLiteral("modelListCalls"),
                       state.value(QStringLiteral("modelListCalls"), 0).toInt() + 1);
        state.sync();
        if (scenario == QStringLiteral("models-error")) {
            std::fputs(R"({"id":1,"result":{"userAgent":"fake"}})" "\n"
                       R"({"id":2,"error":{"code":-32000,"message":"Failed to load model catalog"}})" "\n",
                       stdout);
            std::fflush(stdout);
            return 0;
        }
        const QByteArray response =
            R"({"id":1,"result":{"userAgent":"fake"}})" "\n"
            R"({"id":2,"result":{"data":[)"
            R"({"id":"gpt-6-astra","model":"gpt-6-astra","displayName":"GPT-6-Astra","description":"Frontier model","hidden":false,"isDefault":true},)"
            R"({"id":"gpt-reserve","model":"gpt-reserve","displayName":"GPT-Reserve","description":"Hidden model","hidden":true,"isDefault":false},)"
            R"({"id":"gpt-5.6-sol","model":"gpt-5.6-sol","displayName":"GPT-5.6-Sol","description":"Workhorse model","hidden":false,"isDefault":false},)"
            R"({"id":"gpt-5.6-luna","model":"gpt-5.6-luna","displayName":"GPT-5.6-Luna","description":"Fast model","hidden":false,"isDefault":false})"
            R"(],"nextCursor":null}})" "\n";
        std::fwrite(response.constData(), 1, size_t(response.size()), stdout);
        std::fflush(stdout);
        return 0;
    }

    if (args.contains(QStringLiteral("exec"))) {
        QFile inFile;
        inFile.open(stdin, QIODevice::ReadOnly);
        const QByteArray stdinBytes = inFile.readAll();
        state.setValue(QStringLiteral("lastCodexPrompt"), QString::fromUtf8(stdinBytes));
        state.setValue(QStringLiteral("lastCodexArgs"), args.join(QLatin1Char(' ')));
        const int modelIdx = args.indexOf(QStringLiteral("-m"));
        state.setValue(QStringLiteral("lastCodexModel"),
                       (modelIdx >= 0 && modelIdx + 1 < args.size()) ? args[modelIdx + 1] : QString());
        state.sync();

        const int delayMs = state.value(QStringLiteral("codexDelayMs"), 180).toInt();
        if (delayMs > 0) QThread::msleep(static_cast<unsigned long>(delayMs));

        if (scenario == QStringLiteral("codex-error")) {
            std::fputs("warning: benign warning\nError: model quota exceeded\n", stderr);
            return 1;
        }
        const QByteArray reply = state.value(
            QStringLiteral("codexReply"),
            QStringLiteral("更新 GameConfig 印度区正式服与调试开关配置")).toString().toUtf8();
        std::fwrite(reply.constData(), 1, size_t(reply.size()), stdout);
        std::fflush(stdout);
        return 0;
    }

    // Otherwise simulate p4.exe
    if (args.contains(QStringLiteral("opened"))) {
        state.setValue(QStringLiteral("lastP4OpenedArgs"), args.join(QLatin1Char(' ')));
        state.sync();
        if (scenario == QStringLiteral("empty-changelist")) {
            return 0;
        }
        const QByteArray out =
            "... depotFile //DTS/Branch/FF_PRELive/Client/Assets/Scripts/COWLauncher/GameConfig.cs\n"
            "... action edit\n"
            "... type text\n"
            "... rev 122\n\n"
            "... depotFile //DTS/Branch/FF_PRELive/Client/Assets/Scripts/COWLauncher/IgnoredFile.cs\n"
            "... action edit\n"
            "... type text\n"
            "... rev 5\n\n";
        std::fwrite(out.constData(), 1, size_t(out.size()), stdout);
        std::fflush(stdout);
        return 0;
    }

    if (args.contains(QStringLiteral("diff"))) {
        state.setValue(QStringLiteral("lastP4DiffArgs"), args.join(QLatin1Char(' ')));
        state.sync();
        const QByteArray diffOut =
            "--- //DTS/Branch/FF_PRELive/Client/Assets/Scripts/COWLauncher/GameConfig.cs\n"
            "+++ F:/Perforce/GameConfig.cs\n"
            "@@ -10,1 +10,2 @@\n"
            "-m_LocalConfig = ReadUrlFromLocalConfig();\n"
            "+m_LocalConfig = new LocalConfig();\n"
            "+m_LocalConfig.verAddr = \"https://ffi-version.freefireind.in/live/\";\n";
        std::fwrite(diffOut.constData(), 1, size_t(diffOut.size()), stdout);
        std::fflush(stdout);
        return 0;
    }

    if (args.contains(QStringLiteral("describe"))) {
        if (scenario == QStringLiteral("empty-changelist")) return 0;
    }

    return 0;
}
