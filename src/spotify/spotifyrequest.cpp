/*
 * Strawberry Music Player
 * Copyright 2022-2024, Jonas Kvinge <jonas@jkvinge.net>
 *
 * Strawberry is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Strawberry is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Strawberry.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "config.h"

#include <QObject>
#include <QByteArray>
#include <QString>
#include <QUrl>
#include <QImage>
#include <QImageReader>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QTimer>

#include "core/logging.h"
#include "core/networkaccessmanager.h"
#include "core/song.h"
#include "core/application.h"
#include "utilities/timeconstants.h"
#include "utilities/imageutils.h"
#include "utilities/coverutils.h"
#include "spotifyservice.h"
#include "spotifybaserequest.h"
#include "spotifyrequest.h"

namespace {
const int kMaxConcurrentArtistsRequests = 1;
const int kMaxConcurrentAlbumsRequests = 1;
const int kMaxConcurrentSongsRequests = 1;
const int kMaxConcurrentArtistAlbumsRequests = 1;
const int kMaxConcurrentAlbumSongsRequests = 1;
const int kMaxConcurrentAlbumCoverRequests = 10;
const int kFlushRequestsDelay = 200;
}

SpotifyRequest::SpotifyRequest(SpotifyService *service, Application *app, NetworkAccessManager *network, Type type, QObject *parent)
    : SpotifyBaseRequest(service, network, parent),
      service_(service),
      app_(app),
      network_(network),
      timer_flush_requests_(new QTimer(this)),
      type_(type),
      fetchalbums_(service->fetchalbums()),
      query_id_(-1),
      finished_(false),
      artists_requests_total_(0),
      artists_requests_active_(0),
      artists_requests_received_(0),
      artists_total_(0),
      artists_received_(0),
      albums_requests_total_(0),
      albums_requests_active_(0),
      albums_requests_received_(0),
      albums_total_(0),
      albums_received_(0),
      songs_requests_total_(0),
      songs_requests_active_(0),
      songs_requests_received_(0),
      songs_total_(0),
      songs_received_(0),
      artist_albums_requests_total_(),
      artist_albums_requests_active_(0),
      artist_albums_requests_received_(0),
      artist_albums_total_(0),
      artist_albums_received_(0),
      album_songs_requests_active_(0),
      album_songs_requests_received_(0),
      album_songs_requests_total_(0),
      album_songs_total_(0),
      album_songs_received_(0),
      album_covers_requests_total_(0),
      album_covers_requests_active_(0),
      album_covers_requests_received_(0),
      no_results_(false) {

  timer_flush_requests_->setInterval(kFlushRequestsDelay);
  timer_flush_requests_->setSingleShot(false);
  QObject::connect(timer_flush_requests_, &QTimer::timeout, this, &SpotifyRequest::FlushRequests);

}

SpotifyRequest::~SpotifyRequest() {

  if (timer_flush_requests_->isActive()) {
    timer_flush_requests_->stop();
  }

  while (!replies_.isEmpty()) {
    QNetworkReply *reply = replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

  while (!album_cover_replies_.isEmpty()) {
    QNetworkReply *reply = album_cover_replies_.takeFirst();
    QObject::disconnect(reply, nullptr, this, nullptr);
    if (reply->isRunning()) reply->abort();
    reply->deleteLater();
  }

}

void SpotifyRequest::Process() {

  if (!service_->authenticated()) {
    emit UpdateStatus(query_id_, tr("Authenticating..."));
    return;
  }

  switch (type_) {
    case Type::Artists:
      GetArtists();
      break;
    case Type::Albums:
      GetAlbums();
      break;
    case Type::Songs:
      GetSongs();
      break;
    case Type::SearchArtists:
      ArtistsSearch();
      break;
    case Type::SearchAlbums:
      AlbumsSearch();
      break;
    case Type::SearchSongs:
      SongsSearch();
      break;
    default:
      Error(QStringLiteral("Invalid query type."));
      break;
  }

}

void SpotifyRequest::StartRequests() {

  if (!timer_flush_requests_->isActive()) {
    timer_flush_requests_->start();
  }

}

void SpotifyRequest::FlushRequests() {

  if (!artists_requests_queue_.isEmpty()) {
    FlushArtistsRequests();
    return;
  }

  if (!albums_requests_queue_.isEmpty()) {
    FlushAlbumsRequests();
    return;
  }

  if (!artist_albums_requests_queue_.isEmpty()) {
    FlushArtistAlbumsRequests();
    return;
  }

  if (!album_songs_requests_queue_.isEmpty()) {
    FlushAlbumSongsRequests();
    return;
  }

  if (!songs_requests_queue_.isEmpty()) {
    FlushSongsRequests();
    return;
  }

  if (!album_cover_requests_queue_.isEmpty()) {
    FlushAlbumCoverRequests();
    return;
  }

  timer_flush_requests_->stop();

}

void SpotifyRequest::Search(const int query_id, const QString &search_text) {

  query_id_ = query_id;
  search_text_ = search_text;

}

void SpotifyRequest::GetArtists() {

  emit UpdateStatus(query_id_, tr("Receiving artists..."));
  emit UpdateProgress(query_id_, 0);
  AddArtistsRequest();

}

void SpotifyRequest::AddArtistsRequest(const int offset, const int limit) {

  Request request;
  request.limit = limit;
  request.offset = offset;
  artists_requests_queue_.enqueue(request);

  ++artists_requests_total_;

  StartRequests();

}

void SpotifyRequest::FlushArtistsRequests() {

  while (!artists_requests_queue_.isEmpty() && artists_requests_active_ < kMaxConcurrentArtistsRequests) {

    Request request = artists_requests_queue_.dequeue();

    ParamList parameters = ParamList() << Param(QStringLiteral("type"), QStringLiteral("artist"));
    if (type_ == Type::SearchArtists) {
      parameters << Param(QStringLiteral("q"), search_text_);
    }
    if (request.limit > 0) {
      parameters << Param(QStringLiteral("limit"), QString::number(request.limit));
    }
    if (request.offset > 0) {
      parameters << Param(QStringLiteral("offset"), QString::number(request.offset));
    }
    QNetworkReply *reply = nullptr;
    if (type_ == Type::Artists) {
      reply = CreateRequest(QStringLiteral("me/following"), parameters);
    }
    if (type_ == Type::SearchArtists) {
      reply = CreateRequest(QStringLiteral("search"), parameters);
    }
    if (!reply) continue;
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { ArtistsReplyReceived(reply, request.limit, request.offset); });

    ++artists_requests_active_;

  }

}

void SpotifyRequest::GetAlbums() {

  emit UpdateStatus(query_id_, tr("Receiving albums..."));
  emit UpdateProgress(query_id_, 0);
  AddAlbumsRequest();

}

void SpotifyRequest::AddAlbumsRequest(const int offset, const int limit) {

  Request request;
  request.limit = limit;
  request.offset = offset;
  albums_requests_queue_.enqueue(request);

  ++albums_requests_total_;

  StartRequests();

}

void SpotifyRequest::FlushAlbumsRequests() {

  while (!albums_requests_queue_.isEmpty() && albums_requests_active_ < kMaxConcurrentAlbumsRequests) {

    Request request = albums_requests_queue_.dequeue();

    ParamList parameters;
    if (type_ == Type::SearchAlbums) {
      parameters << Param(QStringLiteral("type"), QStringLiteral("album"));
      parameters << Param(QStringLiteral("q"), search_text_);
    }
    else {
      parameters << Param(QStringLiteral("include_groups"), QStringLiteral("album,single"));
    }
    if (request.limit > 0) parameters << Param(QStringLiteral("limit"), QString::number(request.limit));
    if (request.offset > 0) parameters << Param(QStringLiteral("offset"), QString::number(request.offset));
    QNetworkReply *reply = nullptr;
    if (type_ == Type::Albums) {
      reply = CreateRequest(QStringLiteral("me/albums"), parameters);
    }
    if (type_ == Type::SearchAlbums) {
      reply = CreateRequest(QStringLiteral("search"), parameters);
    }
    if (!reply) continue;
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { AlbumsReplyReceived(reply, request.limit, request.offset); });

    ++albums_requests_active_;

  }

}

void SpotifyRequest::GetSongs() {

  emit UpdateStatus(query_id_, tr("Receiving songs..."));
  emit UpdateProgress(query_id_, 0);
  AddSongsRequest();

}

void SpotifyRequest::AddSongsRequest(const int offset, const int limit) {

  Request request;
  request.limit = limit;
  request.offset = offset;
  songs_requests_queue_.enqueue(request);

  ++songs_requests_total_;

  StartRequests();

}

void SpotifyRequest::FlushSongsRequests() {

  while (!songs_requests_queue_.isEmpty() && songs_requests_active_ < kMaxConcurrentSongsRequests) {

    Request request = songs_requests_queue_.dequeue();

    ParamList parameters;
    if (type_ == Type::SearchSongs) {
      parameters << Param(QStringLiteral("type"), QStringLiteral("track"));
      parameters << Param(QStringLiteral("q"), search_text_);
    }
    if (request.limit > 0) {
      parameters << Param(QStringLiteral("limit"), QString::number(request.limit));
    }
    if (request.offset > 0) {
      parameters << Param(QStringLiteral("offset"), QString::number(request.offset));
    }
    QNetworkReply *reply = nullptr;
    if (type_ == Type::Songs) {
      reply = CreateRequest(QStringLiteral("me/tracks"), parameters);
    }
    if (type_ == Type::SearchSongs) {
      reply = CreateRequest(QStringLiteral("search"), parameters);
    }
    if (!reply) continue;
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { SongsReplyReceived(reply, request.limit, request.offset); });

    ++songs_requests_active_;

  }

}

void SpotifyRequest::ArtistsSearch() {

  emit UpdateStatus(query_id_, tr("Searching..."));
  emit UpdateProgress(query_id_, 0);
  AddArtistsSearchRequest();

}

void SpotifyRequest::AddArtistsSearchRequest(const int offset) {

  AddArtistsRequest(offset, service_->artistssearchlimit());

}

void SpotifyRequest::AlbumsSearch() {

  emit UpdateStatus(query_id_, tr("Searching..."));
  emit UpdateProgress(query_id_, 0);
  AddAlbumsSearchRequest();

}

void SpotifyRequest::AddAlbumsSearchRequest(const int offset) {

  AddAlbumsRequest(offset, service_->albumssearchlimit());

}

void SpotifyRequest::SongsSearch() {

  emit UpdateStatus(query_id_, tr("Searching..."));
  emit UpdateProgress(query_id_, 0);
  AddSongsSearchRequest();

}

void SpotifyRequest::AddSongsSearchRequest(const int offset) {

  AddSongsRequest(offset, service_->songssearchlimit());

}

void SpotifyRequest::ArtistsReplyReceived(QNetworkReply *reply, const int limit_requested, const int offset_requested) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QByteArray data = GetReplyData(reply);

  --artists_requests_active_;
  ++artists_requests_received_;

  if (finished_) return;

  if (data.isEmpty()) {
    ArtistsFinishCheck();
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    ArtistsFinishCheck();
    return;
  }

  if (!json_obj.contains(QLatin1String("artists")) || !json_obj[QLatin1String("artists")].isObject()) {
    Error(QStringLiteral("Json object missing values."), json_obj);
    ArtistsFinishCheck();
    return;
  }
  QJsonObject obj_artists = json_obj[QLatin1String("artists")].toObject();

  if (!obj_artists.contains(QLatin1String("limit")) ||
      !obj_artists.contains(QLatin1String("total")) ||
      !obj_artists.contains(QLatin1String("items"))) {
    Error(QStringLiteral("Json object missing values."), obj_artists);
    ArtistsFinishCheck();
    return;
  }

  int offset = 0;
  if (obj_artists.contains(QLatin1String("offset"))) {
    offset = obj_artists[QLatin1String("offset")].toInt();
  }
  int artists_total = obj_artists[QLatin1String("total")].toInt();

  if (offset_requested == 0) {
    artists_total_ = artists_total;
  }
  else if (artists_total != artists_total_) {
    Error(QStringLiteral("Total returned does not match previous total! %1 != %2").arg(artists_total).arg(artists_total_));
    ArtistsFinishCheck();
    return;
  }

  if (offset != offset_requested) {
    Error(QStringLiteral("Offset returned does not match offset requested! %1 != %2").arg(offset).arg(offset_requested));
    ArtistsFinishCheck();
    return;
  }

  if (offset_requested == 0) {
    emit UpdateProgress(query_id_, GetProgress(artists_received_, artists_total_));
  }

  QJsonValue value_items = ExtractItems(obj_artists);
  if (!value_items.isArray()) {
    ArtistsFinishCheck();
    return;
  }

  QJsonArray array_items = value_items.toArray();
  if (array_items.isEmpty()) {  // Empty array means no results
    if (offset_requested == 0) no_results_ = true;
    ArtistsFinishCheck();
    return;
  }

  int artists_received = 0;
  for (const QJsonValueRef value_item : array_items) {

    ++artists_received;

    if (!value_item.isObject()) {
      Error(QStringLiteral("Invalid Json reply, item in array is not a object."));
      continue;
    }
    QJsonObject obj_item = value_item.toObject();

    if (obj_item.contains(QLatin1String("item"))) {
      QJsonValue json_item = obj_item[QLatin1String("item")];
      if (!json_item.isObject()) {
        Error(QStringLiteral("Invalid Json reply, item in array is not a object."), json_item);
        continue;
      }
      obj_item = json_item.toObject();
    }

    if (!obj_item.contains(QLatin1String("id")) || !obj_item.contains(QLatin1String("name"))) {
      Error(QStringLiteral("Invalid Json reply, item missing id or album."), obj_item);
      continue;
    }

    QString artist_id = obj_item[QLatin1String("id")].toString();
    QString artist = obj_item[QLatin1String("name")].toString();

    if (artist_albums_requests_pending_.contains(artist_id)) continue;

    ArtistAlbumsRequest request;
    request.artist.artist_id = artist_id;
    request.artist.artist = artist;
    artist_albums_requests_pending_.insert(artist_id, request);

  }
  artists_received_ += artists_received;

  if (offset_requested != 0) emit UpdateProgress(query_id_, GetProgress(artists_total_, artists_received_));

  ArtistsFinishCheck(limit_requested, offset, artists_received);

}

void SpotifyRequest::ArtistsFinishCheck(const int limit, const int offset, const int artists_received) {

  if (finished_) return;

  if ((limit == 0 || limit > artists_received) && artists_received_ < artists_total_) {
    int offset_next = offset + artists_received;
    if (offset_next > 0 && offset_next < artists_total_) {
      if (type_ == Type::Artists) AddArtistsRequest(offset_next);
      else if (type_ == Type::SearchArtists) AddArtistsSearchRequest(offset_next);
    }
  }

  if (artists_requests_queue_.isEmpty() && artists_requests_active_ <= 0) {  // Artist query is finished, get all albums for all artists.

    // Get artist albums
    QList<ArtistAlbumsRequest> requests = artist_albums_requests_pending_.values();
    for (const ArtistAlbumsRequest &request : requests) {
      AddArtistAlbumsRequest(request.artist);
    }
    artist_albums_requests_pending_.clear();

    if (artist_albums_requests_total_ > 0) {
      if (artist_albums_requests_total_ == 1) emit UpdateStatus(query_id_, tr("Receiving albums for %1 artist...").arg(artist_albums_requests_total_));
      else emit UpdateStatus(query_id_, tr("Receiving albums for %1 artists...").arg(artist_albums_requests_total_));
      emit UpdateProgress(query_id_, 0);
    }

  }

  FinishCheck();

}

void SpotifyRequest::AlbumsReplyReceived(QNetworkReply *reply, const int limit_requested, const int offset_requested) {

  --albums_requests_active_;
  ++albums_requests_received_;
  AlbumsReceived(reply, Artist(), limit_requested, offset_requested);

}

void SpotifyRequest::AddArtistAlbumsRequest(const Artist &artist, const int offset) {

  ArtistAlbumsRequest request;
  request.artist = artist;
  request.offset = offset;
  artist_albums_requests_queue_.enqueue(request);

  ++artist_albums_requests_total_;

  StartRequests();

}

void SpotifyRequest::FlushArtistAlbumsRequests() {

  while (!artist_albums_requests_queue_.isEmpty() && artist_albums_requests_active_ < kMaxConcurrentArtistAlbumsRequests) {

    ArtistAlbumsRequest request = artist_albums_requests_queue_.dequeue();

    ParamList parameters;
    if (request.offset > 0) parameters << Param(QStringLiteral("offset"), QString::number(request.offset));
    QNetworkReply *reply = CreateRequest(QStringLiteral("artists/%1/albums").arg(request.artist.artist_id), parameters);
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { ArtistAlbumsReplyReceived(reply, request.artist, request.offset); });
    replies_ << reply;

    ++artist_albums_requests_active_;

  }

}

void SpotifyRequest::ArtistAlbumsReplyReceived(QNetworkReply *reply, const Artist &artist, const int offset_requested) {

  --artist_albums_requests_active_;
  ++artist_albums_requests_received_;
  emit UpdateProgress(query_id_, GetProgress(artist_albums_requests_received_, artist_albums_requests_total_));
  AlbumsReceived(reply, artist, 0, offset_requested);

}

void SpotifyRequest::AlbumsReceived(QNetworkReply *reply, const Artist &artist_artist, const int limit_requested, const int offset_requested) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QByteArray data = GetReplyData(reply);

  if (finished_) return;

  if (data.isEmpty()) {
    AlbumsFinishCheck(artist_artist);
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    AlbumsFinishCheck(artist_artist);
    return;
  }

  if (json_obj.contains(QLatin1String("albums")) && json_obj[QLatin1String("albums")].isObject()) {
    json_obj = json_obj[QLatin1String("albums")].toObject();
  }

  if (json_obj.contains(QLatin1String("tracks")) && json_obj[QLatin1String("tracks")].isObject()) {
    json_obj = json_obj[QLatin1String("tracks")].toObject();
  }

  if (!json_obj.contains(QLatin1String("limit")) ||
      !json_obj.contains(QLatin1String("offset")) ||
      !json_obj.contains(QLatin1String("total")) ||
      !json_obj.contains(QLatin1String("items"))) {
    Error(QStringLiteral("Json object missing values."), json_obj);
    AlbumsFinishCheck(artist_artist);
    return;
  }

  int offset = json_obj[QLatin1String("offset")].toInt();
  int albums_total = json_obj[QLatin1String("total")].toInt();

  if (type_ == Type::Albums || type_ == Type::SearchAlbums) {
    albums_total_ = albums_total;
  }

  if (offset != offset_requested) {
    Error(QStringLiteral("Offset returned does not match offset requested! %1 != %2").arg(offset).arg(offset_requested));
    AlbumsFinishCheck(artist_artist);
    return;
  }

  QJsonValue value_items = ExtractItems(json_obj);
  if (!value_items.isArray()) {
    AlbumsFinishCheck(artist_artist);
    return;
  }
  QJsonArray array_items = value_items.toArray();
  if (array_items.isEmpty()) {
    if ((type_ == Type::Albums || type_ == Type::SearchAlbums || (type_ == Type::SearchSongs && fetchalbums_)) && offset_requested == 0) {
      no_results_ = true;
    }
    AlbumsFinishCheck(artist_artist);
    return;
  }

  int albums_received = 0;
  for (const QJsonValueRef value_item : array_items) {

    ++albums_received;

    if (!value_item.isObject()) {
      Error(QStringLiteral("Invalid Json reply, item in array is not a object."));
      continue;
    }
    QJsonObject obj_item = value_item.toObject();

    if (obj_item.contains(QLatin1String("item"))) {
      QJsonValue json_item = obj_item[QLatin1String("item")];
      if (!json_item.isObject()) {
        Error(QStringLiteral("Invalid Json reply, item in array is not a object."), json_item);
        continue;
      }
      obj_item = json_item.toObject();
    }

    if (obj_item.contains(QLatin1String("album"))) {
      QJsonValue json_item = obj_item[QLatin1String("album")];
      if (!json_item.isObject()) {
        Error(QStringLiteral("Invalid Json reply, album in array is not a object."), json_item);
        continue;
      }
      obj_item = json_item.toObject();
    }

    Artist artist;
    Album album;

    if (!obj_item.contains(QLatin1String("id"))) {
      Error(QStringLiteral("Invalid Json reply, item is missing ID."), obj_item);
      continue;
    }
    if (!obj_item.contains(QLatin1String("name"))) {
      Error(QStringLiteral("Invalid Json reply, item is missing name."), obj_item);
      continue;
    }
    if (!obj_item.contains(QLatin1String("images"))) {
      Error(QStringLiteral("Invalid Json reply, item is missing images."), obj_item);
      continue;
    }
    album.album_id = obj_item[QLatin1String("id")].toString();
    album.album = obj_item[QLatin1String("name")].toString();

    if (obj_item.contains(QLatin1String("artists")) && obj_item[QLatin1String("artists")].isArray()) {
      QJsonArray array_artists = obj_item[QLatin1String("artists")].toArray();
      for (const QJsonValueRef value : array_artists) {
        if (!value.isObject()) {
          continue;
        }
        QJsonObject obj_artist = value.toObject();
        if (obj_artist.isEmpty() || !obj_artist.contains(QLatin1String("id")) || !obj_artist.contains(QLatin1String("name"))) continue;
        artist.artist_id = obj_artist[QLatin1String("id")].toString();
        artist.artist = obj_artist[QLatin1String("name")].toString();
        break;
      }
    }

    if (artist.artist_id.isEmpty()) {
      artist = artist_artist;
    }

    if (obj_item.contains(QLatin1String("images")) && obj_item[QLatin1String("images")].isArray()) {
      QJsonArray array_images = obj_item[QLatin1String("images")].toArray();
      for (const QJsonValueRef value : array_images) {
        if (!value.isObject()) {
          continue;
        }
        QJsonObject obj_image = value.toObject();
        if (obj_image.isEmpty() || !obj_image.contains(QLatin1String("url")) || !obj_image.contains(QLatin1String("width")) || !obj_image.contains(QLatin1String("height"))) continue;
        int width = obj_image[QLatin1String("width")].toInt();
        int height = obj_image[QLatin1String("height")].toInt();
        if (width <= 300 || height <= 300) {
          continue;
        }
        album.cover_url = QUrl(obj_image[QLatin1String("url")].toString());
      }
    }

    if (obj_item.contains(QLatin1String("tracks")) && obj_item[QLatin1String("tracks")].isObject()) {
      QJsonObject obj_tracks = obj_item[QLatin1String("tracks")].toObject();
      if (obj_tracks.contains(QLatin1String("items")) && obj_tracks[QLatin1String("items")].isArray()) {
        QJsonArray array_tracks = obj_tracks[QLatin1String("items")].toArray();
        bool compilation = false;
        bool multidisc = false;
        SongList songs;
        for (const QJsonValueRef value : array_tracks) {
          if (!value.isObject()) {
            continue;
          }
          QJsonObject obj_track = value.toObject();
          if (obj_track.contains(QLatin1String("track")) && obj_track[QLatin1String("track")].isObject()) {
            obj_track = obj_track[QLatin1String("track")].toObject();
          }
          Song song(Song::Source::Spotify);
          ParseSong(song, obj_track, artist, album);
          if (!song.is_valid()) continue;
          if (song.disc() >= 2) multidisc = true;
          if (song.is_compilation()) compilation = true;
          songs << song;
        }
        for (Song song : songs) {
          if (compilation) song.set_compilation_detected(true);
          if (!multidisc) song.set_disc(0);
          songs_.insert(song.song_id(), song);
        }
      }
    }
    else if (!album_songs_requests_pending_.contains(album.album_id)) {
      AlbumSongsRequest request;
      request.artist = artist;
      request.album = album;
      album_songs_requests_pending_.insert(album.album_id, request);
    }

  }

  if (type_ == Type::Albums || type_ == Type::SearchAlbums) {
    albums_received_ += albums_received;
    emit UpdateProgress(query_id_, GetProgress(albums_received_, albums_total_));
  }

  AlbumsFinishCheck(artist_artist, limit_requested, offset, albums_total, albums_received);

}

void SpotifyRequest::AlbumsFinishCheck(const Artist &artist, const int limit, const int offset, const int albums_total, const int albums_received) {

  if (finished_) return;

  if (limit == 0 || limit > albums_received) {
    int offset_next = offset + albums_received;
    if (offset_next > 0 && offset_next < albums_total) {
      switch (type_) {
        case Type::Albums:
          AddAlbumsRequest(offset_next);
          break;
        case Type::SearchAlbums:
          AddAlbumsSearchRequest(offset_next);
          break;
        case Type::Artists:
        case Type::SearchArtists:
          AddArtistAlbumsRequest(artist, offset_next);
          break;
        default:
          break;
      }
    }
  }

  if (
      artists_requests_queue_.isEmpty() &&
      artists_requests_active_ <= 0 &&
      albums_requests_queue_.isEmpty() &&
      albums_requests_active_ <= 0 &&
      artist_albums_requests_queue_.isEmpty() &&
      artist_albums_requests_active_ <= 0
      ) { // Artist albums query is finished, get all songs for all albums.

    // Get songs for all the albums.

    for (QMap<QString, AlbumSongsRequest> ::iterator it = album_songs_requests_pending_.begin(); it != album_songs_requests_pending_.end(); ++it) {
      AlbumSongsRequest request = it.value();
      AddAlbumSongsRequest(request.artist, request.album);
    }
    album_songs_requests_pending_.clear();

    if (album_songs_requests_total_ > 0) {
      if (album_songs_requests_total_ == 1) emit UpdateStatus(query_id_, tr("Receiving songs for %1 album...").arg(album_songs_requests_total_));
      else emit UpdateStatus(query_id_, tr("Receiving songs for %1 albums...").arg(album_songs_requests_total_));
      emit UpdateProgress(query_id_, 0);
    }
  }

  GetAlbumCoversCheck();

  FinishCheck();

}

void SpotifyRequest::SongsReplyReceived(QNetworkReply *reply, const int limit_requested, const int offset_requested) {

  --songs_requests_active_;
  ++songs_requests_received_;
  if (type_ == Type::SearchSongs && fetchalbums_) {
    AlbumsReceived(reply, Artist(), limit_requested, offset_requested);
  }
  else {
    SongsReceived(reply, Artist(), Album(), limit_requested, offset_requested);
  }

}

void SpotifyRequest::AddAlbumSongsRequest(const Artist &artist, const Album &album, const int offset) {

  AlbumSongsRequest request;
  request.artist = artist;
  request.album = album;
  request.offset = offset;
  album_songs_requests_queue_.enqueue(request);

  ++album_songs_requests_total_;

  StartRequests();

}

void SpotifyRequest::FlushAlbumSongsRequests() {

  while (!album_songs_requests_queue_.isEmpty() && album_songs_requests_active_ < kMaxConcurrentAlbumSongsRequests) {

    AlbumSongsRequest request = album_songs_requests_queue_.dequeue();
    ++album_songs_requests_active_;
    ParamList parameters;
    if (request.offset > 0) parameters << Param(QStringLiteral("offset"), QString::number(request.offset));
    QNetworkReply *reply = CreateRequest(QStringLiteral("albums/%1/tracks").arg(request.album.album_id), parameters);
    replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { AlbumSongsReplyReceived(reply, request.artist, request.album, request.offset); });

  }

}

void SpotifyRequest::AlbumSongsReplyReceived(QNetworkReply *reply, const Artist &artist, const Album &album, const int offset_requested) {

  --album_songs_requests_active_;
  ++album_songs_requests_received_;
  if (offset_requested == 0) {
    emit UpdateProgress(query_id_, GetProgress(album_songs_requests_received_, album_songs_requests_total_));
  }
  SongsReceived(reply, artist, album, 0, offset_requested);

}

void SpotifyRequest::SongsReceived(QNetworkReply *reply, const Artist &artist, const Album &album, const int limit_requested, const int offset_requested) {

  if (!replies_.contains(reply)) return;
  replies_.removeAll(reply);
  QObject::disconnect(reply, nullptr, this, nullptr);
  reply->deleteLater();

  QByteArray data = GetReplyData(reply);

  if (finished_) return;

  if (data.isEmpty()) {
    SongsFinishCheck(artist, album, limit_requested, offset_requested, 0, 0);
    return;
  }

  QJsonObject json_obj = ExtractJsonObj(data);
  if (json_obj.isEmpty()) {
    SongsFinishCheck(artist, album, limit_requested, offset_requested, 0, 0);
    return;
  }

  if (json_obj.contains(QLatin1String("tracks")) && json_obj[QLatin1String("tracks")].isObject()) {
    json_obj = json_obj[QLatin1String("tracks")].toObject();
  }

  if (!json_obj.contains(QLatin1String("limit")) ||
      !json_obj.contains(QLatin1String("offset")) ||
      !json_obj.contains(QLatin1String("total")) ||
      !json_obj.contains(QLatin1String("items"))) {
    Error(QStringLiteral("Json object missing values."), json_obj);
    SongsFinishCheck(artist, album, limit_requested, offset_requested, 0, 0);
    return;
  }

  int offset = json_obj[QLatin1String("offset")].toInt();
  int songs_total = json_obj[QLatin1String("total")].toInt();

  if (type_ == Type::Songs || type_ == Type::SearchSongs) {
    songs_total_ = songs_total;
  }

  if (offset != offset_requested) {
    Error(QStringLiteral("Offset returned does not match offset requested! %1 != %2").arg(offset).arg(offset_requested));
    SongsFinishCheck(artist, album, limit_requested, offset_requested, songs_total, 0);
    return;
  }

  QJsonValue json_value = ExtractItems(json_obj);
  if (!json_value.isArray()) {
    SongsFinishCheck(artist, album, limit_requested, offset_requested, songs_total, 0);
    return;
  }

  QJsonArray array_items = json_value.toArray();
  if (array_items.isEmpty()) {
    if ((type_ == Type::Songs || type_ == Type::SearchSongs) && offset_requested == 0) {
      no_results_ = true;
    }
    SongsFinishCheck(artist, album, limit_requested, offset_requested, songs_total, 0);
    return;
  }

  bool compilation = false;
  bool multidisc = false;
  SongList songs;
  int songs_received = 0;
  for (const QJsonValueRef value_item : array_items) {

    if (!value_item.isObject()) {
      Error(QStringLiteral("Invalid Json reply, track is not a object."));
      continue;
    }
    QJsonObject obj_item = value_item.toObject();

    if (obj_item.contains(QLatin1String("item")) && obj_item[QLatin1String("item")].isObject()) {
      obj_item = obj_item[QLatin1String("item")].toObject();
    }

    if (obj_item.contains(QLatin1String("track")) && obj_item[QLatin1String("track")].isObject()) {
      obj_item = obj_item[QLatin1String("track")].toObject();
    }

    ++songs_received;
    Song song(Song::Source::Spotify);
    ParseSong(song, obj_item, artist, album);
    if (!song.is_valid()) continue;
    if (song.disc() >= 2) multidisc = true;
    if (song.is_compilation()) compilation = true;
    songs << song;
  }

  for (Song song : songs) {
    if (compilation) song.set_compilation_detected(true);
    if (!multidisc) song.set_disc(0);
    songs_.insert(song.song_id(), song);
  }

  if (type_ == Type::Songs || type_ == Type::SearchSongs) {
    songs_received_ += songs_received;
    emit UpdateProgress(query_id_, GetProgress(songs_received_, songs_total_));
  }

  SongsFinishCheck(artist, album, limit_requested, offset_requested, songs_total, songs_received);

}

void SpotifyRequest::SongsFinishCheck(const Artist &artist, const Album &album, const int limit, const int offset, const int songs_total, const int songs_received) {

  if (finished_) return;

  if (limit == 0 || limit > songs_received) {
    int offset_next = offset + songs_received;
    if (offset_next > 0 && offset_next < songs_total) {
      switch (type_) {
        case Type::Songs:
          AddSongsRequest(offset_next);
          break;
        case Type::SearchSongs:
          // If artist_id and album_id isn't zero it means that it's a songs search where we fetch all albums too. So fallthrough.
          if (artist.artist_id.isEmpty() && album.album_id.isEmpty()) {
            AddSongsSearchRequest(offset_next);
            break;
          }
          // fallthrough
        case Type::Artists:
        case Type::SearchArtists:
        case Type::Albums:
        case Type::SearchAlbums:
          AddAlbumSongsRequest(artist, album, offset_next);
          break;
        default:
          break;
      }
    }
  }

  GetAlbumCoversCheck();

  FinishCheck();

}

void SpotifyRequest::ParseSong(Song &song, const QJsonObject &json_obj, const Artist &album_artist, const Album &album) {

  if (
      !json_obj.contains(QLatin1String("type")) ||
      !json_obj.contains(QLatin1String("id")) ||
      !json_obj.contains(QLatin1String("name")) ||
      !json_obj.contains(QLatin1String("uri")) ||
      !json_obj.contains(QLatin1String("duration_ms")) ||
      !json_obj.contains(QLatin1String("track_number")) ||
      !json_obj.contains(QLatin1String("disc_number"))
    ) {
    Error(QStringLiteral("Invalid Json reply, track is missing one or more values."), json_obj);
    return;
  }

  QString artist_id;
  QString artist_title;
  if (json_obj.contains(QLatin1String("artists")) && json_obj[QLatin1String("artists")].isArray()) {
    QJsonArray array_artists = json_obj[QLatin1String("artists")].toArray();
    for (const QJsonValueRef value_artist : array_artists) {
      if (!value_artist.isObject()) continue;
       QJsonObject obj_artist = value_artist.toObject();
       if (!obj_artist.contains(QLatin1String("type")) || !obj_artist.contains(QLatin1String("id")) || !obj_artist.contains(QLatin1String("name"))) {
         continue;
       }
       artist_id = obj_artist[QLatin1String("id")].toString();
       artist_title = obj_artist[QLatin1String("name")].toString();
       break;
    }
  }

  QString album_id;
  QString album_title;
  QUrl cover_url;
  if (json_obj.contains(QLatin1String("album")) && json_obj[QLatin1String("album")].isObject()) {
    QJsonObject obj_album = json_obj[QLatin1String("album")].toObject();
    if (obj_album.contains(QLatin1String("type")) && obj_album.contains(QLatin1String("id")) && obj_album.contains(QLatin1String("name"))) {
      album_id = obj_album[QLatin1String("id")].toString();
      album_title = obj_album[QLatin1String("name")].toString();
      if (obj_album.contains(QLatin1String("images")) && obj_album[QLatin1String("images")].isArray()) {
        QJsonArray array_images = obj_album[QLatin1String("images")].toArray();
        for (const QJsonValueRef value : array_images) {
          if (!value.isObject()) {
            continue;
          }
          QJsonObject obj_image = value.toObject();
          if (obj_image.isEmpty() || !obj_image.contains(QLatin1String("url")) || !obj_image.contains(QLatin1String("width")) || !obj_image.contains(QLatin1String("height"))) continue;
          int width = obj_image[QLatin1String("width")].toInt();
          int height = obj_image[QLatin1String("height")].toInt();
          if (width <= 300 || height <= 300) {
            continue;
          }
          cover_url = QUrl(obj_image[QLatin1String("url")].toString());
        }
      }
    }
  }

  if (artist_id.isEmpty() || artist_title.isEmpty()) {
    artist_id = album_artist.artist_id;
    artist_title = album_artist.artist;
  }

  if (album_id.isEmpty() || album_title.isEmpty() || cover_url.isEmpty()) {
    album_id = album.album_id;
    album_title = album.album;
    cover_url = album.cover_url;
  }

  QString song_id = json_obj[QLatin1String("id")].toString();
  QString title = json_obj[QLatin1String("name")].toString();
  QString uri = json_obj[QLatin1String("uri")].toString();
  qint64 duration = json_obj[QLatin1String("duration_ms")].toVariant().toLongLong() * kNsecPerMsec;
  int track = json_obj[QLatin1String("track_number")].toInt();
  int disc = json_obj[QLatin1String("disc_number")].toInt();

  QUrl url(uri);

  title = Song::TitleRemoveMisc(title);

  song.set_source(Song::Source::Spotify);
  song.set_song_id(song_id);
  song.set_album_id(album_id);
  song.set_artist_id(artist_id);
  if (album_artist.artist != artist_title) {
    song.set_albumartist(album_artist.artist);
  }
  song.set_album(album_title);
  song.set_artist(artist_title);
  song.set_title(title);
  song.set_track(track);
  song.set_disc(disc);
  song.set_url(url);
  song.set_length_nanosec(duration);
  song.set_art_automatic(cover_url);
  song.set_directory_id(0);
  song.set_filetype(Song::FileType::Stream);
  song.set_filesize(0);
  song.set_mtime(0);
  song.set_ctime(0);
  song.set_valid(true);

}

void SpotifyRequest::GetAlbumCoversCheck() {

  if (
      !finished_ &&
      service_->download_album_covers() &&
      IsQuery() &&
      artists_requests_queue_.isEmpty() &&
      albums_requests_queue_.isEmpty() &&
      songs_requests_queue_.isEmpty() &&
      artist_albums_requests_queue_.isEmpty() &&
      album_songs_requests_queue_.isEmpty() &&
      album_cover_requests_queue_.isEmpty() &&
      artist_albums_requests_pending_.isEmpty() &&
      album_songs_requests_pending_.isEmpty() &&
      album_covers_requests_sent_.isEmpty() &&
      artists_requests_active_ <= 0 &&
      albums_requests_active_ <= 0 &&
      songs_requests_active_ <= 0 &&
      artist_albums_requests_active_ <= 0 &&
      album_songs_requests_active_ <= 0 &&
      album_covers_requests_active_ <= 0
  ) {
    GetAlbumCovers();
  }

}

void SpotifyRequest::GetAlbumCovers() {

  const SongList songs = songs_.values();
  for (const Song &song : songs) {
    AddAlbumCoverRequest(song);
  }

  if (album_covers_requests_total_ == 1) emit UpdateStatus(query_id_, tr("Receiving album cover for %1 album...").arg(album_covers_requests_total_));
  else emit UpdateStatus(query_id_, tr("Receiving album covers for %1 albums...").arg(album_covers_requests_total_));
  emit UpdateProgress(query_id_, 0);

  StartRequests();

}

void SpotifyRequest::AddAlbumCoverRequest(const Song &song) {

  if (album_covers_requests_sent_.contains(song.album_id())) {
    album_covers_requests_sent_.insert(song.album_id(), song.song_id());
    return;
  }

  AlbumCoverRequest request;
  request.album_id = song.album_id();
  request.url = song.art_automatic();
  request.filename = CoverUtils::CoverFilePath(CoverOptions(), song.source(), song.effective_albumartist(), song.effective_album(), song.album_id(), QString(), request.url);
  if (request.filename.isEmpty()) return;

  album_covers_requests_sent_.insert(song.album_id(), song.song_id());
  ++album_covers_requests_total_;

  album_cover_requests_queue_.enqueue(request);

}

void SpotifyRequest::FlushAlbumCoverRequests() {

  while (!album_cover_requests_queue_.isEmpty() && album_covers_requests_active_ < kMaxConcurrentAlbumCoverRequests) {

    AlbumCoverRequest request = album_cover_requests_queue_.dequeue();

    QNetworkRequest req(request.url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply *reply = network_->get(req);
    album_cover_replies_ << reply;
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, request]() { AlbumCoverReceived(reply, request.album_id, request.url, request.filename); });

    ++album_covers_requests_active_;

  }

}

void SpotifyRequest::AlbumCoverReceived(QNetworkReply *reply, const QString &album_id, const QUrl &url, const QString &filename) {

  if (album_cover_replies_.contains(reply)) {
    album_cover_replies_.removeAll(reply);
    QObject::disconnect(reply, nullptr, this, nullptr);
    reply->deleteLater();
  }
  else {
    AlbumCoverFinishCheck();
    return;
  }

  --album_covers_requests_active_;
  ++album_covers_requests_received_;

  if (finished_) return;

  emit UpdateProgress(query_id_, GetProgress(album_covers_requests_received_, album_covers_requests_total_));

  if (!album_covers_requests_sent_.contains(album_id)) {
    AlbumCoverFinishCheck();
    return;
  }

  if (reply->error() != QNetworkReply::NoError) {
    Error(QStringLiteral("%1 (%2)").arg(reply->errorString()).arg(reply->error()));
    album_covers_requests_sent_.remove(album_id);
    AlbumCoverFinishCheck();
    return;
  }

  if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200) {
    Error(QStringLiteral("Received HTTP code %1 for %2.").arg(reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()).arg(url.toString()));
    if (album_covers_requests_sent_.contains(album_id)) album_covers_requests_sent_.remove(album_id);
    AlbumCoverFinishCheck();
    return;
  }

  QString mimetype = reply->header(QNetworkRequest::ContentTypeHeader).toString();
  if (mimetype.contains(QLatin1Char(';'))) {
    mimetype = mimetype.left(mimetype.indexOf(QLatin1Char(';')));
  }
  if (!ImageUtils::SupportedImageMimeTypes().contains(mimetype, Qt::CaseInsensitive) && !ImageUtils::SupportedImageFormats().contains(mimetype, Qt::CaseInsensitive)) {
    Error(QStringLiteral("Unsupported mimetype for image reader %1 for %2").arg(mimetype, url.toString()));
    if (album_covers_requests_sent_.contains(album_id)) album_covers_requests_sent_.remove(album_id);
    AlbumCoverFinishCheck();
    return;
  }

  QByteArray data = reply->readAll();
  if (data.isEmpty()) {
    Error(QStringLiteral("Received empty image data for %1").arg(url.toString()));
    if (album_covers_requests_sent_.contains(album_id)) album_covers_requests_sent_.remove(album_id);
    AlbumCoverFinishCheck();
    return;
  }

  QList<QByteArray> format_list = QImageReader::imageFormatsForMimeType(mimetype.toUtf8());
  char *format = nullptr;
  if (!format_list.isEmpty()) {
    format = format_list.first().data();
  }

  QImage image;
  if (image.loadFromData(data, format)) {
    if (image.save(filename, format)) {
      while (album_covers_requests_sent_.contains(album_id)) {
        const QString song_id = album_covers_requests_sent_.take(album_id);
        if (songs_.contains(song_id)) {
          songs_[song_id].set_art_automatic(QUrl::fromLocalFile(filename));
        }
      }
    }
    else {
      Error(QStringLiteral("Error saving image data to %1").arg(filename));
      if (album_covers_requests_sent_.contains(album_id)) album_covers_requests_sent_.remove(album_id);
    }
  }
  else {
    if (album_covers_requests_sent_.contains(album_id)) album_covers_requests_sent_.remove(album_id);
    Error(QStringLiteral("Error decoding image data from %1").arg(url.toString()));
  }

  AlbumCoverFinishCheck();

}

void SpotifyRequest::AlbumCoverFinishCheck() {

  FinishCheck();

}

void SpotifyRequest::FinishCheck() {

  if (
      !finished_ &&
      artists_requests_queue_.isEmpty() &&
      albums_requests_queue_.isEmpty() &&
      songs_requests_queue_.isEmpty() &&
      artist_albums_requests_queue_.isEmpty() &&
      album_songs_requests_queue_.isEmpty() &&
      album_cover_requests_queue_.isEmpty() &&
      artist_albums_requests_pending_.isEmpty() &&
      album_songs_requests_pending_.isEmpty() &&
      album_covers_requests_sent_.isEmpty() &&
      artists_requests_active_ <= 0 &&
      albums_requests_active_ <= 0 &&
      songs_requests_active_ <= 0 &&
      artist_albums_requests_active_ <= 0 &&
      album_songs_requests_active_ <= 0 &&
      album_covers_requests_active_ <= 0
  ) {
    if (timer_flush_requests_->isActive()) {
      timer_flush_requests_->stop();
    }
    finished_ = true;
    if (no_results_ && songs_.isEmpty()) {
      if (IsSearch())
        emit Results(query_id_, SongMap(), tr("No match."));
      else
        emit Results(query_id_, SongMap(), QString());
    }
    else {
      if (songs_.isEmpty() && errors_.isEmpty()) {
        emit Results(query_id_, songs_, tr("Data missing error"));
      }
      else {
        emit Results(query_id_, songs_, ErrorsToHTML(errors_));
      }
    }
  }

}

int SpotifyRequest::GetProgress(const int count, const int total) {

  return static_cast<int>((static_cast<float>(count) / static_cast<float>(total)) * 100.0F);

}

void SpotifyRequest::Error(const QString &error, const QVariant &debug) {

  if (!error.isEmpty()) {
    errors_ << error;
    qLog(Error) << "Spotify:" << error;
  }

  if (debug.isValid()) qLog(Debug) << debug;

  FinishCheck();

}

void SpotifyRequest::Warn(const QString &error, const QVariant &debug) {

  qLog(Error) << "Spotify:" << error;
  if (debug.isValid()) qLog(Debug) << debug;

}
