#include "engine_positions.h"
#include "engine/crengine/reader_model.h"
#include "reading/kosync_location.h"
#include "support/strings.h"
#include <lvdocview.h>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QUrl>

namespace kobalt::nickel {
namespace {
QString qstring(const lString32 &s) {
    return QString::fromUtf8(UnicodeToUtf8(s).c_str());
}
lString32 cre(const QString &s) {
    return Utf8ToUnicode(s.toUtf8().constData());
}
// Nickel point() locations count DOM children from the document element and
// measure offsets in UTF-8 bytes. Resolve against the original XML before
// checking that CREngine retained the identified element's exact text.
bool nativePoint(ldomDocument *doc, const QString &path, const QString &point, QString &id, int &offset) {
    static const QRegularExpression format(QStringLiteral("^point\\((/1(?:/[1-9][0-9]*)*)(?::([0-9]+))?\\)$"));
    const auto match = format.match(point);
    if (!match.hasMatch())
        return false;
    auto source = doc->getContainer()->OpenStream(cre(path).c_str(), LVOM_READ);
    if (source.isNull() || source->GetSize() > 16 * 1024 * 1024)
        return false;
    QByteArray xml(int(source->GetSize()), '\0');
    lvsize_t read = 0;
    if (source->Read(xml.data(), xml.size(), &read) != LVERR_OK || read != lvsize_t(xml.size()))
        return false;
    struct Node {
        QString id;
        QVector<int> children;
        int begin = 0, end = 0, parent = -1;
        bool text = false, cdata = false, element = false;
    };
    QVector<Node> nodes(1);
    QVector<int> stack{0};
    QString text;
    int root = -1;
    bool adjacentText = false;
    QXmlStreamReader parser(xml);
    while (!parser.atEnd()) {
        parser.readNext();
        if (!parser.isCharacters())
            adjacentText = false;
        if (parser.isStartElement() || parser.isCharacters() || parser.isComment()) {
            const int parent = stack.last();
            // Adjacent character tokens belong to one DOM text node.
            if (parser.isCharacters() && adjacentText && !nodes[parent].children.isEmpty() &&
                nodes[nodes[parent].children.last()].text && !parser.isCDATA() &&
                !nodes[nodes[parent].children.last()].cdata) {
                text += parser.text();
                nodes[nodes[parent].children.last()].end = text.size();
                continue;
            }
            const int index = nodes.size();
            Node node;
            node.begin = text.size();
            node.parent = parent;
            node.text = parser.isCharacters();
            node.cdata = parser.isCDATA();
            node.element = parser.isStartElement();
            if (parser.isStartElement())
                node.id = parser.attributes().value("id").toString();
            if (parser.isCharacters())
                text += parser.text();
            adjacentText = parser.isCharacters();
            node.end = text.size();
            nodes.append(node);
            nodes[parent].children.append(index);
            if (parser.isStartElement()) {
                if (parent == 0)
                    root = index;
                stack.append(index);
            }
        } else if (parser.isEndElement()) {
            nodes[stack.takeLast()].end = text.size();
        }
    }
    if (parser.hasError() || root < 0)
        return false;
    int target = root;
    const auto sequence = match.captured(1).split('/');
    for (int i = 2; i < sequence.size(); ++i) {
        bool ok = false;
        const int child = sequence[i].toInt(&ok) - 1;
        if (!ok || child < 0 || child >= nodes[target].children.size())
            return false;
        target = nodes[target].children[child];
    }
    if (!nodes[target].text && !nodes[target].element)
        return false;
    bool ok = true;
    const int bytes = match.captured(2).isEmpty() ? 0 : match.captured(2).toInt(&ok);
    const auto targetText = text.mid(nodes[target].begin, nodes[target].end - nodes[target].begin);
    const auto utf8 = targetText.toUtf8();
    if (!ok || bytes > utf8.size())
        return false;
    const auto prefix = QString::fromUtf8(utf8.constData(), bytes);
    if (prefix.toUtf8() != utf8.left(bytes))
        return false;
    const int position = nodes[target].begin + prefix.size();
    for (int node = target; node > 0; node = nodes[node].parent) {
        if (!nodes[node].id.isEmpty()) {
            id = nodes[node].id;
            offset = position - nodes[node].begin;
            return true;
        }
    }
    // A native element location can point into a descendant with its own ID.
    for (int node = target + 1; node < nodes.size(); ++node) {
        if (nodes[node].begin > position)
            break;
        if (!nodes[node].id.isEmpty() && nodes[node].begin <= position && nodes[node].end >= position) {
            int ancestor = node;
            while (ancestor > 0 && ancestor != target)
                ancestor = nodes[ancestor].parent;
            if (ancestor == target) {
                id = nodes[node].id;
                offset = position - nodes[node].begin;
                return true;
            }
        }
    }
    return false;
}
bool nativeText(ldomDocument *doc, const QString &path, const QString &id, ldomNode *element, QString &sourceText,
                QString *error, bool *textChanged = nullptr) {
    if (textChanged) {
        *textChanged = false;
    }
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    auto archive = doc->getContainer();
    auto source = archive->OpenStream(cre(path).c_str(), LVOM_READ);
    if (source.isNull() || source->GetSize() > 16 * 1024 * 1024)
        return fail(strings::get("error.native_chapter_source_is_unavailable"));
    QByteArray xml(int(source->GetSize()), '\0');
    lvsize_t read = 0;
    if (source->Read(xml.data(), xml.size(), &read) != LVERR_OK || read != lvsize_t(xml.size()))
        return fail(strings::get("error.native_chapter_source_is_incomplete"));
    QXmlStreamReader parser(xml);
    sourceText.clear();
    int matches = 0, depth = 0, targetDepth = 0;
    while (!parser.atEnd()) {
        parser.readNext();
        if (parser.isStartElement()) {
            ++depth;
            if (parser.attributes().value("id") == id && ++matches == 1)
                targetDepth = depth;
        } else if (parser.isCharacters() && targetDepth) {
            sourceText += parser.text();
        } else if (parser.isEndElement()) {
            if (depth == targetDepth)
                targetDepth = 0;
            --depth;
        }
    }
    if (parser.hasError() || matches != 1) {
        return fail(strings::get("error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    }
    if (sourceText != qstring(element->getText())) {
        if (textChanged) {
            *textChanged = true;
        }
        return fail(strings::get("error.native_text_anchor_cannot_be_mapped_without_changing_its_text"));
    }
    return true;
}
}
class NickelPositionCodec {
public:
    static QString decodeKosync(const ReaderModel &model, const QString &location, QString *error);
    static QString decode(const ReaderModel &model, const QString &location, QString *error);
    static QString encode(const ReaderModel &model, QString *error);
};
// A sync server's position is already a CREngine XPointer, so it needs no
// translation, only a check that this document can place it. When it cannot, as
// after the book or the engine changed, the fraction of the book picks a page.
QString NickelPositionCodec::decodeKosync(const ReaderModel &model, const QString &location, QString *error) {
    QString xpointer;
    double fraction = 0;
    if (!kosync::splitLocation(location, &xpointer, &fraction)) {
        if (error)
            *error = strings::get("error.no_supported_native_reading_position");
        return {};
    }
    if (!xpointer.isEmpty() && !model.engine_->getDocument()->createXPointer(cre(xpointer)).isNull())
        return xpointer;
    const int pages = model.pages();
    const auto start =
        pages > 0 && fraction > 0 ? model.pageStart(qBound(0, int(fraction * pages + 0.5) - 1, pages - 1)) : QString();
    if (start.isEmpty() && error)
        *error = strings::get("error.native_text_position_was_not_found");
    return start;
}
QString NickelPositionCodec::decode(const ReaderModel &model, const QString &location, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!model.engine_ || location.isEmpty() || location.size() > 8192)
        return fail(strings::get("error.no_supported_native_reading_position"));
    if (kosync::isLocation(location))
        return decodeKosync(model, location, error);
    const int hash = location.indexOf('#');
    const auto path = QUrl::fromPercentEncoding(location.left(hash < 0 ? location.size() : hash).toUtf8());
    QString id = hash < 0 ? QString() : location.mid(hash + 1);
    int offset = 0;
    const int colon = id.lastIndexOf(':');
    if (id.startsWith("point(")) {
        if (!nativePoint(model.engine_->getDocument(), path, id, id, offset))
            return fail(strings::get("error.unsupported_native_text_offset"));
    } else if (colon >= 0) {
        bool ok = false;
        offset = id.mid(colon + 1).toInt(&ok);
        if (!ok || offset < 0)
            return fail(strings::get("error.unsupported_native_text_offset"));
        id.truncate(colon);
    }
    id = QUrl::fromPercentEncoding(id.toUtf8());
    auto *doc = model.engine_->getDocument();
    auto *body = doc->nodeFromXPath(U"/body");
    if (!body)
        return fail(strings::get("error.missing_publication_body"));
    ldomNode *fragment = nullptr;
    for (int i = 0; i < body->getChildCount(); ++i) {
        auto *node = body->getChildNode(i);
        if (node->isElement() && node->isNodeName("DocFragment") &&
            qstring(node->getAttributeValue("Source")) == path) {
            if (fragment)
                return fail(strings::get("error.ambiguous_native_chapter_path"));
            fragment = node;
        }
    }
    if (!fragment)
        return fail(strings::get("error.native_chapter_is_not_in_the_publication_spine"));
    if (id.isEmpty()) {
        if (offset)
            return fail(strings::get("error.invalid_native_chapter_position"));
        return qstring(ldomXPointer(fragment, 0).toString());
    }
    const auto internalId = qstring(fragment->getAttributeValue("id")) + "_ " + id;
    auto *element = doc->getNodeById(doc->getAttrValueIndex(cre(internalId).c_str()));
    if (!element)
        return fail(strings::get("error.native_text_anchor_is_not_in_the_publication"));

    // TENG offsets count UTF-16 units in the source DOM. CREngine uses UTF-32
    // and can normalize whitespace. Reject a changed text stream rather than
    // turning the same number into a position in different text.
    QString sourceText;
    if (!nativeText(doc, path, id, element, sourceText, error))
        return {};
    if (offset > sourceText.size())
        return fail(strings::get("error.native_text_offset_exceeds_its_anchor"));
    QVector<ldomNode *> pending{element};
    int remaining = offset;
    while (!pending.isEmpty()) {
        auto *node = pending.takeLast();
        if (node->isText()) {
            const auto text = node->getText();
            int units = 0;
            for (int i = 0; i <= text.length(); ++i) {
                if (units == remaining) {
                    return qstring(ldomXPointer(node, i).toString());
                }
                if (i < text.length())
                    units += text[i] > 0xffff ? 2 : 1;
                if (units > remaining)
                    return fail(strings::get("error.native_offset_splits_a_unicode_character"));
            }
            remaining -= units;
        } else {
            for (int i = node->getChildCount() - 1; i >= 0; --i)
                pending.append(node->getChildNode(i));
        }
    }
    if (sourceText.isEmpty() && !offset)
        return qstring(ldomXPointer(element, 0).toString());
    return fail(strings::get("error.native_text_position_was_not_found"));
}
QString NickelPositionCodec::encode(const ReaderModel &model, QString *error) {
    auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    if (error)
        error->clear();
    if (!model.engine_)
        return fail(strings::get("error.no_reading_position"));
    auto *doc = model.engine_->getDocument();
    const auto pointer = doc->createXPointer(cre(model.anchor_));
    auto *target = pointer.getNode();
    if (!target)
        return fail(strings::get("error.invalid_source_position"));
    auto *fragment = target;
    while (fragment && !fragment->isNodeName("DocFragment"))
        fragment = fragment->getParentNode();
    if (!fragment)
        return fail(strings::get("error.position_has_no_source_chapter"));
    const auto path = qstring(fragment->getAttributeValue("Source"));
    if (path.isEmpty())
        return fail(strings::get("error.position_has_no_source_path"));
    const auto encodedPath = QString::fromLatin1(QUrl::toPercentEncoding(path, "/"));
    const auto chapterFallback = [&] {
        auto *body = doc->nodeFromXPath(U"/body");
        if (!body) {
            return fail(strings::get("error.missing_publication_body"));
        }
        for (int i = 0; i < body->getChildCount(); ++i) {
            auto *node = body->getChildNode(i);
            if (node != fragment && node->isElement() && node->isNodeName("DocFragment") &&
                qstring(node->getAttributeValue("Source")) == path) {
                return fail(strings::get("error.ambiguous_native_chapter_path"));
            }
        }
        // Keep the exact local anchor. Nickel gets a valid chapter start when
        // an unlabelled page or normalized text cannot supply an exact offset.
        // This still lets it update the active book, cover and percentage.
        if (error) {
            error->clear();
        }
        return encodedPath;
    };
    if (target == fragment && pointer.getOffset() == 0)
        return encodedPath;
    const auto prefix = qstring(fragment->getAttributeValue("id")) + "_ ";
    auto *element = target->isText() ? target->getParentNode() : target;
    while (element && element != fragment && element->getAttributeValue("id").empty())
        element = element->getParentNode();
    if (!element || element == fragment) {
        return chapterFallback();
    }
    const auto internalId = qstring(element->getAttributeValue("id"));
    if (!internalId.startsWith(prefix)) {
        return chapterFallback();
    }
    const auto id = internalId.mid(prefix.size());
    QString sourceText;
    bool textChanged = false;
    if (!nativeText(doc, path, id, element, sourceText, error, &textChanged)) {
        return textChanged ? chapterFallback() : QString();
    }
    int units = 0;
    QVector<ldomNode *> pending{element};
    while (!pending.isEmpty()) {
        auto *node = pending.takeLast();
        if (node == target) {
            const int offset = pointer.getOffset();
            if (node->isText()) {
                const auto text = node->getText();
                if (offset < 0 || offset > text.length())
                    return fail(strings::get("error.invalid_source_text_offset"));
                for (int i = 0; i < offset; ++i)
                    units += text[i] > 0xffff ? 2 : 1;
            } else {
                // An element XPointer counts children, not characters.
                if (offset < 0 || offset > node->getChildCount())
                    return fail(strings::get("error.invalid_source_element_offset"));
                for (int i = 0; i < offset; ++i)
                    units += qstring(node->getChildNode(i)->getText()).size();
            }
            return encodedPath + '#' + QString::fromLatin1(QUrl::toPercentEncoding(id)) + ':' + QString::number(units);
        }
        if (node->isText())
            units += qstring(node->getText()).size();
        else
            for (int i = node->getChildCount() - 1; i >= 0; --i)
                pending.append(node->getChildNode(i));
    }
    return fail(strings::get("error.source_position_is_outside_its_anchor"));
}
PositionCodec positionCodec() {
    return {NickelPositionCodec::decode, NickelPositionCodec::encode, [](const QString &location) {
                QString xpointer;
                double fraction = 0;
                if (kosync::splitLocation(location, &xpointer, &fraction)) {
                    // The start of the DocFragment holding the XPointer is the start of its chapter.
                    static const QRegularExpression fragment(QStringLiteral("^/body/DocFragment(\\[[0-9]+\\])?"));
                    const auto match = fragment.match(xpointer);
                    return match.hasMatch() ? kosync::makeLocation(match.captured(0), fraction) : QString();
                }
                return location.left(location.indexOf('#') < 0 ? location.size() : location.indexOf('#'));
            }};
}
}
