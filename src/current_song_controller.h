#ifndef CURRENT_SONG_CONTROLLER_H_
#define CURRENT_SONG_CONTROLLER_H_

#include <string>
#include <boost/asio/deadline_timer.hpp>
#include <boost/filesystem.hpp>
#include "nlohmann/json_fwd.hpp"

#include "player_events_ifc.h"
#include "player_actions_ifc.h"
#include "mqtt_api.h"
#include "web_sockets_api.h"
#include "services/alsa_service.h"

using json = nlohmann::json;

namespace wavplayeralsa {

    class CurrentSongController : 
        public PlayerEventsIfc,
        public CurrentSongActionsIfc
    {

    public:
        CurrentSongController(boost::asio::io_service &io_service, 
            MqttApi *mqtt_service, 
            WebSocketsApi *ws_service, 
            AlsaPlaybackServiceFactory *alsa_playback_service_factory);

        void Initialize(const std::string &player_uuid, const std::string &wav_dir);

    public:
        void NewSongStatus(const std::string &file_id, uint32_t play_seq_id, uint64_t start_time_millis_since_epoch, double speed);
        void NoSongPlayingStatus(const std::string &file_id, uint32_t play_seq_id);

    public:

    	// wavplayeralsa::CurrentSongActionsIfc

		bool NewSongRequest(
            const std::string &file_id, 
            int64_t start_offset_ms, 
            std::stringstream &out_msg,
            uint32_t *play_seq_id);

		bool StopPlayRequest(
            std::stringstream &out_msg,
            uint32_t *play_seq_id);

    private:
        void UpdateLastStatusMsg(const json &alsa_data, uint32_t play_seq_id);
        void ReportCurrentSongToServices(const boost::system::error_code& error);

    private:
        boost::asio::io_service &ios_;
        MqttApi *mqtt_service_;
        WebSocketsApi *ws_service_;
        AlsaPlaybackServiceFactory *alsa_playback_service_factory_;
        IAlsaPlaybackService *alsa_service_ = nullptr;

    private:
        // static config
        std::string player_uuid_;
        boost::filesystem::path wav_dir_;

    private:
    	std::string last_status_msg_;
        uint32_t play_seq_id_;

    private:
        // throttling issues:
        const int THROTTLE_WAIT_TIME_MS = 50;
        boost::asio::deadline_timer throttle_timer_;
        bool throttle_timer_set_ = false;

    };
}

#endif // CURRENT_SONG_CONTROLLER_H_
