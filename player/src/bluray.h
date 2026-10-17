#pragma once

#include <QString>

// Blu-ray disc layouts on the filesystem: a folder holding BDMV/ with the
// playlist, clip-information and clip streams. Menus are deliberately out of
// scope - the shell only ever wants the main feature.
namespace bluray {

// True when `path` is a directory that looks like a disc root. The names are
// index.bdmv and MovieObject.bdmv - with the trailing v.
bool isDisc(const QString &path);

// The biggest BDMV/STREAM/*.m2ts, as an absolute path. Used as the source of the
// frame strip, where one real clip is what a single still needs.
QString longestClip(const QString &path);

// The clips of the main feature, in playlist order: the .mpls whose referenced
// clips add up to the most bytes wins. A pressed disc keeps the feature and the
// bonus material in separate playlists, and libbluray itself picks the longest
// title (measured: with the short title at idx 0 the disc still opened on the
// long one), so this mirrors that choice for the fallback route.
QStringList mainFeatureClips(const QString &path);

// Every BDMV/STREAM clip in name order, written out as an m3u the player can load
// as one continuous title. The fallback cannot assume the film is one clip: a
// pressed disc splits a feature across several, and the biggest of them is only
// a slice of the movie. Returns an empty string when nothing was written.
QString clipPlaylist(const QString &path, const QString &cacheDir);

// Total bytes of BDMV/STREAM, for the library's size column.
qint64 streamBytes(const QString &path);

} // namespace bluray
