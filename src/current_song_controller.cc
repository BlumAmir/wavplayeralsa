#include <current_song_controller.h>

#include <iostream>
#include <boost/bind.hpp>
#include "nlohmann/json.hpp"

using json = nlohmann::json;

namespace wavplayeralsa 
{

    CurrentSongController::CurrentSongController(
			boost::asio::io_service &io_service, 
			MqttApi *mqtt_service,  
			WebSocketsApi *ws_service,
			AlsaPlaybackServiceFactory *alsa_playback_service_factory
		) : 
			ios_(io_service), 
			mqtt_service_(mqtt_service),
			ws_service_(ws_service),
			alsa_playback_service_factory_(alsa_playback_service_factory),
			play_seq_id_(0),
			throttle_timer_(io_service)
    {

    }

    void CurrentSongController::Initialize(const std::string &player_uuid, const std::string &wav_dir)
    {
        player_uuid_ = player_uuid;
        wav_dir_ = boost::filesystem::path(wav_dir);

        json j;
		j["song_is_playing"] = false;
		UpdateLastStatusMsg(j, play_seq_id_);
    }

    void CurrentSongController::NewSongStatus(const std::string &file_id, uint32_t play_seq_id, uint64_t start_time_millis_since_epoch, double speed)
    {
		json j;
		j["song_is_playing"] = true;
		j["file_id"] = file_id;
		j["start_time_millis_since_epoch"] = start_time_millis_since_epoch;
		j["speed"] = speed;

        ios_.post(std::bind(&CurrentSongController::UpdateLastStatusMsg, this, j, play_seq_id));
    }

    void CurrentSongController::NoSongPlayingStatus(const std::string &file_id, uint32_t play_seq_id)       
    {
		json j;
		j["song_is_playing"] = false;
		j["stopped_file_id"] = file_id;
        
        ios_.post(std::bind(&CurrentSongController::UpdateLastStatusMsg, this, j, play_seq_id));
    }

	bool CurrentSongController::NewSongRequest(
        const std::string &file_id, 
        int64_t start_offset_ms, 
        std::stringstream &out_msg,
        uint32_t *play_seq_id) 
    {
		bool prev_file_was_playing = false;
		std::string prev_file_id;

		if(alsa_service_ != nullptr) {
			prev_file_id = alsa_service_->GetFileId();
			prev_file_was_playing = alsa_service_->Stop();
			delete alsa_service_;
			alsa_service_ = nullptr;
		}

		// create a new unique id for this play
		uint32_t new_play_seq_id = play_seq_id_ + 1;
        play_seq_id_ = new_play_seq_id;
        if(play_seq_id != nullptr)
        {
            *play_seq_id = play_seq_id_;
        }

		boost::filesystem::path songPathInWavDir(file_id);
		boost::filesystem::path songFullPath = wav_dir_ / songPathInWavDir;
		std::string canonicalFullPath;
		try {
			canonicalFullPath = boost::filesystem::canonical(songFullPath).string();
			alsa_service_ = alsa_playback_service_factory_->CreateAlsaPlaybackService(
				canonicalFullPath, 
				file_id,
				new_play_seq_id
			);
		}
		catch(const std::runtime_error &e) {
			out_msg << "failed loading new audio file '" << file_id << "'. currently no audio file is loaded in the player and it is not playing. " <<
				"reason for failure: " << e.what();
			return false;
		}

		if(file_id == prev_file_id) {
			out_msg << "changed position of the current file '" << file_id << "'. new position in ms is: " << start_offset_ms << std::endl;
		}
		else {
			static const int SECONDS_PER_HOUR = (60 * 60);
			uint64_t start_offset_sec = std::abs(start_offset_ms) / 1000;
			uint64_t hours = start_offset_sec / SECONDS_PER_HOUR;
			start_offset_sec = start_offset_sec - hours * SECONDS_PER_HOUR;
			uint64_t minutes = start_offset_sec / 60;
			uint64_t seconds = start_offset_sec % 60;
			if(prev_file_was_playing && !prev_file_id.empty()) {
				out_msg << "audio file successfully changed from '" << prev_file_id << "' to '" << file_id << "' and will be played ";
			}
			else {
				out_msg << "will play audio file '" << file_id << "' ";
			}
			out_msg << "starting at position " << start_offset_ms << " ms " <<
				"(" << hours << ":" << 
				std::setfill('0') << std::setw(2) << minutes << ":" << 
				std::setfill('0') << std::setw(2) << seconds;
			if(start_offset_ms < 0) {
				out_msg << " in the future";
			}
			out_msg << ")";
		}

        try {
			alsa_service_->Play(start_offset_ms);
        }
        catch(const std::runtime_error &e) {
            out_msg << "playing new audio file '" << file_id << "' failed. currently player is not playing. " <<
                "reason for failure: " << e.what();
            return false;
        }

		return true;
	}

	bool CurrentSongController::StopPlayRequest(
        std::stringstream &out_msg,
        uint32_t *play_seq_id) 
    {
		bool was_playing = false;
		std::string current_file_id;
		if(alsa_service_ != nullptr) {
			current_file_id = alsa_service_->GetFileId();
			was_playing = alsa_service_->Stop();
			delete alsa_service_;
			alsa_service_ = nullptr;
		}

		if(current_file_id.empty() || !was_playing) {
			out_msg << "no audio file is being played, so stop had no effect";			
		}
		else {
			out_msg << "current audio file '" << current_file_id << "' stopped playing";				
		}

        if(play_seq_id != nullptr)
        {
            *play_seq_id = play_seq_id_;
        }

		return true;
	}

	void CurrentSongController::UpdateLastStatusMsg(const json &alsa_data, uint32_t play_seq_id)
	{
        json full_msg(alsa_data);
        full_msg["uuid"] = player_uuid_;
        full_msg["play_seq_id"] = play_seq_id;

		const std::string msg_json_str = full_msg.dump();

		if(msg_json_str == last_status_msg_) {
			return;
		}

		last_status_msg_ = msg_json_str;

        if(!throttle_timer_set_) {
            throttle_timer_.expires_from_now(boost::posix_time::milliseconds(THROTTLE_WAIT_TIME_MS));
            throttle_timer_.async_wait(boost::bind(&CurrentSongController::ReportCurrentSongToServices, this, _1));			
            throttle_timer_set_ = true;
        }
	}

    void CurrentSongController::ReportCurrentSongToServices(const boost::system::error_code& error)
    {
        throttle_timer_set_ = false;
        if(error)
            return;

        mqtt_service_->ReportCurrentSong(last_status_msg_);
        ws_service_->ReportCurrentSong(last_status_msg_);        
    }

}


