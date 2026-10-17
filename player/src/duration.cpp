#include "duration.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>

#include <cmath>
#include <cstring>

namespace {

constexpr qint64 kHeadBytes = 64 * 1024;

double credible(double seconds)
{
    if (!std::isfinite(seconds) || seconds <= 0.0)
        return 0.0;
    // Anything past three days is a misread field, not a feature length film.
    if (seconds > 3.0 * 86400.0)
        return 0.0;
    return seconds;
}

quint16 be16(const uchar *p) { return quint16((p[0] << 8) | p[1]); }
quint32 be32(const uchar *p)
{
    return (quint32(p[0]) << 24) | (quint32(p[1]) << 16) | (quint32(p[2]) << 8) | p[3];
}
quint64 be64(const uchar *p) { return (quint64(be32(p)) << 32) | be32(p + 4); }
quint32 le32(const uchar *p)
{
    return (quint32(p[3]) << 24) | (quint32(p[2]) << 16) | (quint32(p[1]) << 8) | p[0];
}

// ---------------------------------------------------------------- Matroska/WebM

struct Elem {
    quint64 id = 0;
    qsizetype body = 0;
    qsizetype end = 0;
};

// EBML element header: an ID and a size, both "vints" whose length is implied by
// the leading zero bits of the first byte. *len reports that length so callers
// can recognise the all-ones "unknown size" value.
bool readVint(const QByteArray &b, qsizetype *p, qsizetype limit, quint64 *out,
              bool keepMarker, int *len = nullptr)
{
    if (*p >= limit)
        return false;
    const uchar first = uchar(b.at(*p));
    int n = 0;
    for (int bit = 7; bit >= 0; --bit) {
        if (first & (1 << bit))
            break;
        ++n;
    }
    ++n;
    if (n > 8 || *p + n > limit)
        return false;
    quint64 v = keepMarker ? first : quint64(first & (0xFF >> n));
    for (int i = 1; i < n; ++i)
        v = (v << 8) | uchar(b.at(*p + i));
    *p += n;
    *out = v;
    if (len)
        *len = n;
    return true;
}

bool nextElem(const QByteArray &b, qsizetype *p, qsizetype limit, Elem *out)
{
    Elem e;
    quint64 size = 0;
    int slen = 0;
    if (!readVint(b, p, limit, &e.id, true))
        return false;
    if (!readVint(b, p, limit, &size, false, &slen))
        return false;
    // An "unknown" size has every value bit set; the element then runs to the
    // end of its parent, which the clamp below already is.
    const quint64 allOnes = (Q_UINT64_C(1) << (7 * slen)) - 1;
    const bool unknown = size == allOnes;
    e.body = *p;
    e.end = unknown ? limit : qMin(e.body + qsizetype(size), limit);
    if (e.end <= e.body)
        return false;
    *p = e.end;
    *out = e;
    return true;
}

double readUint(const QByteArray &b, const Elem &e)
{
    quint64 v = 0;
    for (qsizetype i = e.body; i < e.end && i - e.body < 8; ++i)
        v = (v << 8) | uchar(b.at(i));
    return double(v);
}

double readFloat(const QByteArray &b, const Elem &e)
{
    const qsizetype n = e.end - e.body;
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + e.body;
    if (n == 4) {
        const quint32 raw = be32(p);
        float f = 0.0f;
        std::memcpy(&f, &raw, 4);
        return double(f);
    }
    if (n == 8) {
        const quint64 raw = be64(p);
        double d = 0.0;
        std::memcpy(&d, &raw, 8);
        return d;
    }
    return 0.0;
}

double ebmlDuration(const QByteArray &b)
{
    qsizetype p = 0;
    Elem seg;
    while (nextElem(b, &p, b.size(), &seg)) {
        if (seg.id != 0x18538067ull)       // Segment
            continue;
        qsizetype q = seg.body;
        Elem info;
        while (nextElem(b, &q, seg.end, &info)) {
            if (info.id != 0x1549A966ull)  // Info
                continue;
            double scale = 1000000.0;      // TimecodeScale defaults to 1 ms
            double frames = 0.0;           // Duration, in timecode units
            qsizetype r = info.body;
            Elem e;
            while (nextElem(b, &r, info.end, &e)) {
                if (e.id == 0x2AD7B1ull)
                    scale = readUint(b, e);
                else if (e.id == 0x4489ull)
                    frames = readFloat(b, e);
            }
            return frames > 0.0 ? credible(frames * scale / 1e9) : 0.0;
        }
        return 0.0;
    }
    return 0.0;
}

// ------------------------------------------------------------- MP4 / QuickTime

bool readBox(QFile &f, quint32 *type, qint64 *body, qint64 *end)
{
    const qint64 size = f.size();
    const qint64 start = f.pos();
    if (start + 8 > size)
        return false;
    uchar hdr[8];
    if (f.read(reinterpret_cast<char *>(hdr), 8) != 8)
        return false;
    quint64 total = be32(hdr);
    qint64 bodyAt = start + 8;
    if (total == 1) {
        uchar big[8];
        if (f.read(reinterpret_cast<char *>(big), 8) != 8)
            return false;
        total = be64(big);
        bodyAt = start + 16;
    } else if (total == 0) {
        total = quint64(size - start);
    }
    if (total < 8 || bodyAt >= size)
        return false;
    *type = (quint32(hdr[4]) << 24) | (quint32(hdr[5]) << 16) | (quint32(hdr[6]) << 8) | hdr[7];
    *body = bodyAt;
    *end = qMin<qint64>(start + qint64(total), size);
    return true;
}

double mvhdDuration(QFile &f, qint64 moovBody, qint64 moovEnd)
{
    f.seek(moovBody);
    for (int guard = 0; guard < 512; ++guard) {
        if (f.pos() >= moovEnd)
            break;
        const qint64 start = f.pos();
        quint32 type = 0;
        qint64 body = 0, end = 0;
        if (!readBox(f, &type, &body, &end))
            return 0.0;
        if (type == 0x6D766864u) {   // 'mvhd'
            uchar buf[32];
            f.seek(body);
            const qint64 got = f.read(reinterpret_cast<char *>(buf), 32);
            if (got < 20)
                return 0.0;
            const uchar ver = buf[0];
            double timescale = 0.0, duration = 0.0;
            if (ver == 0) {
                timescale = be32(buf + 12);
                duration = be32(buf + 16);
            } else {
                if (got < 32)
                    return 0.0;
                timescale = be32(buf + 20);
                duration = double(be64(buf + 24));
            }
            return (timescale > 0.0 && duration > 0.0) ? credible(duration / timescale) : 0.0;
        }
        f.seek(end);
        if (f.pos() <= start)
            return 0.0;
    }
    return 0.0;
}

double mp4Duration(QFile &f)
{
    f.seek(0);
    for (int guard = 0; guard < 4096; ++guard) {
        const qint64 start = f.pos();
        if (start >= f.size())
            break;
        quint32 type = 0;
        qint64 body = 0, end = 0;
        if (!readBox(f, &type, &body, &end))
            return 0.0;
        if (type == 0x6D6F6F76u)     // 'moov'
            return mvhdDuration(f, body, end);
        f.seek(end);
        if (f.pos() <= start)
            return 0.0;
    }
    return 0.0;
}

// -------------------------------------------------------------- RIFF / AVI, FLAC

double aviDuration(const QByteArray &b)
{
    const qsizetype i = b.indexOf("avih");
    if (i < 0 || i + 8 + 20 > b.size())
        return 0.0;
    const uchar *d = reinterpret_cast<const uchar *>(b.constData()) + i + 8;
    const quint32 usPerFrame = le32(d);
    const quint32 frames = le32(d + 16);
    if (!usPerFrame || !frames)
        return 0.0;
    return credible(double(usPerFrame) * double(frames) / 1e6);
}

double flacDuration(const QByteArray &b)
{
    // "fLaC", then the STREAMINFO block header (1 flag byte + 3 size bytes).
    if (b.size() < 8 + 18)
        return 0.0;
    const uchar *p = reinterpret_cast<const uchar *>(b.constData()) + 8;
    // STREAMINFO: minBlock 2 + maxBlock 2 + minFrame 3 + maxFrame 3, then a word
    // of sampleRate:20 | channels:3 | bitsPerSample:5 | totalSamples high 4 bits,
    // then the low 32 bits of totalSamples.
    const quint32 word = be32(p + 10);
    const quint32 rate = word >> 12;
    const quint64 samples = (quint64(word & 0x0F) << 32) | be32(p + 14);
    if (!rate || !samples)
        return 0.0;
    return credible(double(samples) / double(rate));
}

} // namespace

double readDuration(const QString &path)
{
    // A disc folder is deliberately 0: the MPLS TotalPlayTime and the CLPI
    // PlayTime are not filled in reliably (measured on a genuine tsMuxeR-authored
    // disc: 150 ticks for a 60 s film). The catalogue learns the real length
    // when the disc is opened.
    const QFileInfo info(path);
    if (info.isDir())
        return 0.0;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0.0;
    if (f.size() < 16) {
        f.close();
        return 0.0;
    }
    QByteArray head = f.read(kHeadBytes);
    f.close();
    if (head.size() < 16)
        return 0.0;

    const uchar *p = reinterpret_cast<const uchar *>(head.constData());
    if (be32(p) == 0x1A45DFA3u)
        return ebmlDuration(head);
    if (head.mid(4, 4) == "ftyp") {
        QFile g(path);
        if (!g.open(QIODevice::ReadOnly))
            return 0.0;
        const double s = mp4Duration(g);
        g.close();
        return s;
    }
    if (head.startsWith("RIFF") && head.mid(8, 4) == "AVI ")
        return aviDuration(head);
    if (head.startsWith("fLaC"))
        return flacDuration(head);
    return 0.0;
}
