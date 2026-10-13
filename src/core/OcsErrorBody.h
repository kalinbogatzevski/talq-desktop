#pragma once

// What a failed OCS request answered, read out of its body.
//
// Talk refuses a request with an HTTP error status and an OCS envelope whose `data` is the
// machine-readable reason -- {"ocs":{"meta":{"statuscode":400,"message":""},"data":{"error":"send-at"}}}.
// `meta.message` is usually empty. Qt reports such a reply as a network error, so the body has to
// be read explicitly: ApiClient::handleReply does that and hands `data` to the callback together
// with ok=false, which is what lets a caller say WHY it failed instead of "(HTTP 400)".

#include <QByteArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

namespace talq {

struct OcsError {
    QString     message;   // ocs.meta.message; "" when absent
    QJsonObject data;      // ocs.data when it is an object; {} for an array, a scalar, no body, not JSON
};

inline OcsError parseOcsError(const QByteArray &body)
{
    OcsError e;
    if (body.isEmpty())
        return e;
    const QJsonObject ocs = QJsonDocument::fromJson(body).object()
                                .value(QLatin1String("ocs")).toObject();
    e.message = ocs.value(QLatin1String("meta")).toObject()
                    .value(QLatin1String("message")).toString();
    e.data    = ocs.value(QLatin1String("data")).toObject();
    return e;
}

} // namespace talq
