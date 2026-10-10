#include "camera_sync_state.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>

#include <cmath>
#include <utility>

namespace meshcompare {
namespace {
constexpr int LegacyStateSchema = 1;
constexpr int StateSchema = 2;
constexpr int MaximumStoredBoards = 32;
// Each compact board base is limited to 2 MiB. Keep a bounded history for
// several board keys while leaving room for the state envelope.
constexpr qint64 MaximumStateBytes = 72 * 1024 * 1024;
constexpr qint64 MaximumExactJsonInteger = (qint64(1) << 53) - 1;

OperationResult failure(const QString& message)
{
    return OperationResult::failure(message);
}

OperationResult readRoot(const QString& path, QJsonObject* root, bool* found)
{
    if (root == nullptr || found == nullptr)
        return failure(QStringLiteral("No destination was provided for cloud sync state."));
    *root = {};
    *found = false;
    QFile input(path);
    if (!input.exists())
        return OperationResult::success();
    if (!input.open(QIODevice::ReadOnly))
        return failure(QStringLiteral("Cloud sync state could not be read: %1").arg(input.errorString()));
    if (input.size() > MaximumStateBytes)
        return failure(QStringLiteral("Cloud sync state is unexpectedly large."));
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(input.readAll(), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return failure(QStringLiteral("Cloud sync state is not valid JSON."));
    *root = document.object();
    *found = true;
    return OperationResult::success();
}

OperationResult decodeStateEntry(
    const QString& boardKey,
    const QJsonObject& encoded,
    CameraSyncPersistedState* state)
{
    if (!encoded.value(QStringLiteral("boardVersion")).isDouble() ||
        !encoded.value(QStringLiteral("base")).isObject()) {
        return failure(QStringLiteral("Cloud sync state has an unsupported format."));
    }
    const double encodedVersion =
        encoded.value(QStringLiteral("boardVersion")).toDouble(-1.0);
    if (!std::isfinite(encodedVersion) || encodedVersion < 1.0 ||
        encodedVersion > static_cast<double>(MaximumExactJsonInteger) ||
        std::floor(encodedVersion) != encodedVersion) {
        return failure(QStringLiteral("Cloud sync state has an invalid board version."));
    }
    const QJsonValue syncedAtUtc = encoded.value(QStringLiteral("syncedAtUtc"));
    if (!syncedAtUtc.isUndefined() && !syncedAtUtc.isString())
        return failure(QStringLiteral("Cloud sync state has an unsupported format."));
    CameraPoseSyncDocument base;
    const OperationResult parsed = parseCameraPoseSyncDocument(
        QJsonDocument(encoded.value(QStringLiteral("base")).toObject()).toJson(QJsonDocument::Compact),
        &base);
    if (!parsed.ok)
        return failure(QStringLiteral("Cloud sync state is invalid: %1").arg(parsed.error));
    if (state != nullptr) {
        state->boardKey = boardKey;
        state->boardVersion = static_cast<qint64>(encodedVersion);
        state->syncedAtUtc = syncedAtUtc.toString();
        state->base = std::move(base);
    }
    return OperationResult::success();
}

OperationResult legacyBoards(const QJsonObject& root, QJsonObject* boards)
{
    if (!root.value(QStringLiteral("boardKey")).isString() ||
        root.value(QStringLiteral("boardKey")).toString().isEmpty()) {
        return failure(QStringLiteral("Cloud sync state has an unsupported format."));
    }
    QJsonObject entry = root;
    entry.remove(QStringLiteral("schema"));
    entry.remove(QStringLiteral("boardKey"));
    const QString boardKey = root.value(QStringLiteral("boardKey")).toString();
    const OperationResult validated = decodeStateEntry(boardKey, entry, nullptr);
    if (!validated.ok)
        return validated;
    boards->insert(boardKey, entry);
    return OperationResult::success();
}

OperationResult decodeBoards(const QJsonObject& root, QJsonObject* boards)
{
    const int schema = root.value(QStringLiteral("schema")).toInt(-1);
    if (schema == LegacyStateSchema)
        return legacyBoards(root, boards);
    if (schema != StateSchema || !root.value(QStringLiteral("boards")).isObject())
        return failure(QStringLiteral("Cloud sync state has an unsupported format."));
    const QJsonObject encodedBoards = root.value(QStringLiteral("boards")).toObject();
    for (auto it = encodedBoards.constBegin(); it != encodedBoards.constEnd(); ++it) {
        if (it.key().isEmpty() || !it.value().isObject())
            return failure(QStringLiteral("Cloud sync state has an unsupported format."));
        const OperationResult validated =
            decodeStateEntry(it.key(), it.value().toObject(), nullptr);
        if (!validated.ok)
            return validated;
    }
    *boards = encodedBoards;
    return OperationResult::success();
}

QJsonObject encodeStateEntry(
    const CameraSyncPersistedState& state,
    const QJsonObject& base)
{
    QJsonObject entry;
    entry.insert(
        QStringLiteral("boardVersion"),
        static_cast<double>(state.boardVersion));
    entry.insert(QStringLiteral("syncedAtUtc"), state.syncedAtUtc);
    entry.insert(QStringLiteral("base"), base);
    return entry;
}

void pruneOldestBoards(
    QJsonObject* boards,
    const QString& currentBoardKey)
{
    while (boards->size() > MaximumStoredBoards) {
        QString oldestKey;
        QString oldestTime;
        for (auto it = boards->constBegin(); it != boards->constEnd(); ++it) {
            if (it.key() == currentBoardKey)
                continue;
            const QString syncedAt = it.value().toObject()
                .value(QStringLiteral("syncedAtUtc")).toString();
            if (oldestKey.isEmpty() || syncedAt < oldestTime ||
                (syncedAt == oldestTime && it.key() < oldestKey)) {
                oldestKey = it.key();
                oldestTime = syncedAt;
            }
        }
        if (oldestKey.isEmpty())
            return;
        boards->remove(oldestKey);
    }
}
} // namespace

CameraSyncStateStore::CameraSyncStateStore(QString path)
    : path_(std::move(path))
{
}

QString CameraSyncStateStore::path() const
{
    return path_;
}

OperationResult CameraSyncStateStore::load(
    const QString& boardKey,
    CameraSyncPersistedState* state,
    bool* found) const
{
    if (state == nullptr)
        return failure(QStringLiteral("No destination was provided for cloud sync state."));
    *state = {};
    if (found != nullptr)
        *found = false;

    QJsonObject root;
    bool fileFound = false;
    const OperationResult read = readRoot(path_, &root, &fileFound);
    if (!read.ok || !fileFound)
        return read;
    QJsonObject boards;
    const OperationResult decoded = decodeBoards(root, &boards);
    if (!decoded.ok)
        return decoded;
    const QJsonValue encoded = boards.value(boardKey);
    if (encoded.isUndefined())
        return OperationResult::success();
    if (!encoded.isObject())
        return failure(QStringLiteral("Cloud sync state has an unsupported format."));
    const OperationResult parsed = decodeStateEntry(boardKey, encoded.toObject(), state);
    if (!parsed.ok)
        return parsed;
    if (found != nullptr)
        *found = true;
    return OperationResult::success();
}

OperationResult CameraSyncStateStore::save(
    const CameraSyncPersistedState& state) const
{
    if (path_.isEmpty() || state.boardKey.isEmpty() || state.boardVersion < 1 ||
        state.boardVersion > MaximumExactJsonInteger) {
        return failure(QStringLiteral("Cloud sync state is incomplete."));
    }
    QByteArray baseBytes;
    const OperationResult serialized =
        serializeCameraPoseSyncDocument(state.base, &baseBytes);
    if (!serialized.ok)
        return serialized;
    QJsonParseError parseError;
    const QJsonDocument baseDocument = QJsonDocument::fromJson(baseBytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !baseDocument.isObject())
        return failure(QStringLiteral("Cloud sync state could not be encoded."));

    QJsonObject boards;
    QJsonObject existingRoot;
    bool existingFound = false;
    const OperationResult read = readRoot(path_, &existingRoot, &existingFound);
    if (!read.ok)
        return read;
    if (existingFound) {
        const OperationResult decoded = decodeBoards(existingRoot, &boards);
        if (!decoded.ok)
            return decoded;
    }
    boards.insert(state.boardKey, encodeStateEntry(state, baseDocument.object()));
    pruneOldestBoards(&boards, state.boardKey);

    QJsonObject root;
    root.insert(QStringLiteral("schema"), StateSchema);
    root.insert(QStringLiteral("boards"), boards);
    const QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (bytes.size() > MaximumStateBytes)
        return failure(QStringLiteral("Cloud sync state exceeds its local size limit."));
    const QString parent = QFileInfo(path_).absolutePath();
    if (parent.isEmpty() || !QDir().mkpath(parent))
        return failure(QStringLiteral("Cloud sync state folder could not be created."));
    QSaveFile output(path_);
    if (!output.open(QIODevice::WriteOnly))
        return failure(QStringLiteral("Cloud sync state could not be written: %1").arg(output.errorString()));
    if (output.write(bytes) != bytes.size()) {
        const QString error = output.errorString();
        output.cancelWriting();
        return failure(error.isEmpty()
            ? QStringLiteral("Cloud sync state could not be written.")
            : error);
    }
    if (!output.commit())
        return failure(QStringLiteral("Cloud sync state could not be replaced: %1").arg(output.errorString()));
    return OperationResult::success();
}

} // namespace meshcompare
