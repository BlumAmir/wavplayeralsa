#include "services/alsa_service.h"

#include <sstream>
#include <iostream>
#include <functional>

#include <boost/asio.hpp>

#include "alsa/asoundlib.h"
#include "sndfile.hh"
#include "spdlog/spdlog.h"
#include "spdlog/async.h"

namespace wavplayeralsa
{

    class AlsaPlaybackService : public IAlsaPlaybackService
    {

    public:

        AlsaPlaybackService(
            std::shared_ptr<spdlog::logger> logger,
			PlayerEventsIfc *player_events_callback_,
            const std::string &full_file_name, 
            const std::string &file_id,
			const std::string &audio_device,
			uint32_t play_seq_id
        );

		~AlsaPlaybackService();

	public:
		void Play(int64_t offset_in_ms);
		bool Stop();
		const std::string GetFileId() const { return file_id_; }

    private:

        void InitSndFile(const std::string &full_file_name);  
		void InitAlsa(const std::string &audio_device);      

	private:
		void PlayingThreadMain();
		void FramesToPcmTransferLoop(boost::system::error_code error_code);
		void PcmDrainLoop(boost::system::error_code error_code);
		void PcmDrop();
		void CheckSongStartTime();
		bool IsAlsaStatePlaying();

    private:
        std::shared_ptr<spdlog::logger> logger_;
		boost::asio::io_service ios_;
		boost::asio::deadline_timer alsa_wait_timer_;
		std::thread playing_thread_;
		bool initialized_ = false;

	// config
	private:
		const std::string file_id_;
		const uint32_t play_seq_id_;

    // alsa
    private:
    	static const int TRANSFER_BUFFER_SIZE = 4096 * 16; // 64KB this is the buffer used to pass frames to alsa. this is the maximum number of bytes to pass as one chunk
		snd_pcm_t *alsa_playback_handle_ = nullptr;

		// what is the next frame to be delivered to alsa
		int64_t curr_position_frames_ = 0;

    // snd file
    private:
    	SndfileHandle snd_file_;

	    enum SampleType {
	    	SampleTypeSigned = 0,
	    	SampleTypeUnsigned = 1,
	    	SampleTypeFloat = 2
	    };
	    static const char *SampleTypeToString(SampleType sample_type);
		bool GetFormatForAlsa(snd_pcm_format_t &out_format) const;

		// from file
	    unsigned int frame_rate_ = 44100;
	    unsigned int num_of_channels_ = 2;
	    bool is_endian_little_ = true; // if false the endian is big :)
	    SampleType sample_type_ = SampleTypeSigned;
	    unsigned int bytes_per_sample_ = 2;
	    uint64_t total_frame_in_file_ = 0;

	    // calculated
		unsigned int bytes_per_frame_ = 1;
		snd_pcm_sframes_t frames_capacity_in_buffer_ = 0; // how many frames can be stored in a buffer with size TRANSFER_BUFFER_SIZE

	// postions reporting
	private:
		uint64_t audio_start_time_ms_since_epoch_ = 0;
		PlayerEventsIfc *player_events_callback_ = nullptr;
		
    };

    AlsaPlaybackService::AlsaPlaybackService(
            std::shared_ptr<spdlog::logger> logger,
			PlayerEventsIfc *player_events_callback,
            const std::string &full_file_name, 
            const std::string &file_id,
			const std::string &audio_device,
			uint32_t play_seq_id
        ) :
			file_id_(file_id),
			play_seq_id_(play_seq_id),
            logger_(logger),
			alsa_wait_timer_(ios_),
			player_events_callback_(player_events_callback)
    {
        InitSndFile(full_file_name);
		InitAlsa(audio_device);
		initialized_ = true;
    }

	AlsaPlaybackService::~AlsaPlaybackService() {

		this->Stop();

		if(alsa_playback_handle_ != nullptr) {
			snd_pcm_close(alsa_playback_handle_);
			alsa_playback_handle_ = nullptr;
		}

	}

	/*
	Read the file content from disk, extract relevant metadata from the
	wav header, and save it to the relevant members of the class.
	The function will also initialize the snd_file member, which allows to
	read the wav file frames.
	Will throw std::runtime_error in case of error.
	 */
    void AlsaPlaybackService::InitSndFile(const std::string &full_file_name)
    {
		snd_file_ = SndfileHandle(full_file_name);
		if(snd_file_.error() != 0) {
			std::stringstream errorDesc;
			errorDesc << "The file '" << full_file_name << "' cannot be opened. error msg: '" << snd_file_.strError() << "'";
			throw std::runtime_error(errorDesc.str());
		}

		// set the parameters from read from the SndFile and produce log messages

		frame_rate_ = snd_file_.samplerate();
		num_of_channels_ = snd_file_.channels();

		int major_type = snd_file_.format() & SF_FORMAT_TYPEMASK;
		int minor_type = snd_file_.format() & SF_FORMAT_SUBMASK;

		switch(minor_type) {
			case SF_FORMAT_PCM_S8: 
				bytes_per_sample_ = 1;
				sample_type_ = SampleTypeSigned;
				break;
			case SF_FORMAT_PCM_16: 
				bytes_per_sample_ = 2;
				sample_type_ = SampleTypeSigned;
				break;
			case SF_FORMAT_PCM_24: 
				bytes_per_sample_ = 3;
				sample_type_ = SampleTypeSigned;
				break;
			case SF_FORMAT_PCM_32: 
				bytes_per_sample_ = 4;
				sample_type_ = SampleTypeSigned;
				break;
			case SF_FORMAT_FLOAT:
				bytes_per_sample_ = 4;
				sample_type_ = SampleTypeFloat;
				break;
			case SF_FORMAT_DOUBLE:
				bytes_per_sample_ = 8;
				sample_type_ = SampleTypeFloat;
				break;
			default:
				std::stringstream err_desc;
				err_desc << "wav file is in unsupported format. minor format as read from sndFile is: " << std::hex << minor_type;
				throw std::runtime_error(err_desc.str());
		}

		switch(major_type) {
			case SF_FORMAT_WAV:
				is_endian_little_ = true;
				break;
			case SF_FORMAT_AIFF:
				is_endian_little_ = false;
				break;
			default:
				std::stringstream err_desc;
				err_desc << "wav file is in unsupported format. major format as read from sndFile is: " << std::hex << major_type;
				throw std::runtime_error(err_desc.str());
		}

		total_frame_in_file_ = snd_file_.frames();
		uint64_t number_of_ms = total_frame_in_file_ * 1000 / frame_rate_;
		int number_of_minutes = number_of_ms / (1000 * 60);
		int seconds_modulo = (number_of_ms / 1000) % 60;	

		bytes_per_frame_ = num_of_channels_ * bytes_per_sample_;
		frames_capacity_in_buffer_ = (snd_pcm_sframes_t)(TRANSFER_BUFFER_SIZE / bytes_per_frame_);

		logger_->info("finished reading audio file '{}'. "
			"Frame rate: {} frames per seconds, "
			"Number of channels: {}, "
			"Wav format: major 0x{:x}, minor 0x{:x}, "
			"Bytes per sample: {}, "
			"Sample type: '{}', "
			"Endian: '{}', "
			"Total frames in file: {} which are: {} ms, and {}:{} minutes", 
				full_file_name, frame_rate_, num_of_channels_, major_type, minor_type, bytes_per_sample_, 
				SampleTypeToString(sample_type_), 
				(is_endian_little_ ? "little" : "big"),
				total_frame_in_file_, number_of_ms, number_of_minutes, seconds_modulo
			);

    }

	const char *AlsaPlaybackService::SampleTypeToString(SampleType sample_type) {
		switch(sample_type) {
	    	case SampleTypeSigned: return "signed integer";
	    	case SampleTypeUnsigned: return "unsigned integer";
	    	case SampleTypeFloat: return "float";
		}
		std::stringstream err_desc;
		err_desc << "sample type not supported. value is " << (int)sample_type;
		throw std::runtime_error(err_desc.str());
	}

	/*
	Init the alsa driver according to the params of the current wav file.
	throw std::runtime_error in case of error
	 */
	void AlsaPlaybackService::InitAlsa(const std::string &audio_device) {

		int err;
		std::stringstream err_desc;

		if( (err = snd_pcm_open(&alsa_playback_handle_, audio_device.c_str(), SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
			err_desc << "cannot open audio device " << audio_device << " (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		// set hw parameters

		snd_pcm_hw_params_t *hw_params;

		if( (err = snd_pcm_hw_params_malloc(&hw_params)) < 0 ) {
			err_desc << "cannot allocate hardware parameter structure (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		if( (err = snd_pcm_hw_params_any(alsa_playback_handle_, hw_params)) < 0) {
			err_desc << "cannot initialize hardware parameter structure (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		if( (err = snd_pcm_hw_params_set_access(alsa_playback_handle_, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
			err_desc << "cannot set access type (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		snd_pcm_format_t alsaFormat;
		if(GetFormatForAlsa(alsaFormat) != true) {
			err_desc << "the wav format is not supported by this player of alsa";
			throw std::runtime_error(err_desc.str());
		}
		if( (err = snd_pcm_hw_params_set_format(alsa_playback_handle_, hw_params, alsaFormat)) < 0) {
			err_desc << "cannot set sample format (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		if( (err = snd_pcm_hw_params_set_rate(alsa_playback_handle_, hw_params, frame_rate_, 0)) < 0) {
			err_desc << "cannot set sample rate (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		if( (err = snd_pcm_hw_params_set_channels(alsa_playback_handle_, hw_params, num_of_channels_)) < 0) {
			err_desc << "cannot set channel count (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		if( (err = snd_pcm_hw_params(alsa_playback_handle_, hw_params)) < 0) {
			err_desc << "cannot set alsa hw parameters (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		snd_pcm_hw_params_free(hw_params);
		hw_params = nullptr;


		// set software parameters

		snd_pcm_sw_params_t *sw_params;

		if( (err = snd_pcm_sw_params_malloc(&sw_params)) < 0) {
			err_desc << "cannot allocate software parameters structure (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		if( (err = snd_pcm_sw_params_current(alsa_playback_handle_, sw_params)) < 0) {
			err_desc << "cannot initialize software parameters structure (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		// how many frames should be in the buffer before alsa start to play it.
		// we set to 0 -> means start playing immediately
		if( (err = snd_pcm_sw_params_set_start_threshold(alsa_playback_handle_, sw_params, 0U)) < 0) {
			err_desc << "cannot set start mode (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}	

		if( (err = snd_pcm_sw_params(alsa_playback_handle_, sw_params)) < 0) {
			err_desc << "cannot set software parameters (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}	

		snd_pcm_sw_params_free(sw_params);
		sw_params = nullptr;

		if( (err = snd_pcm_prepare(alsa_playback_handle_)) < 0) {
			err_desc << "cannot prepare audio interface for use (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}

		initialized_ = true;	
	}

	bool AlsaPlaybackService::GetFormatForAlsa(snd_pcm_format_t &out_format) const {
		switch(sample_type_) {

			case SampleTypeSigned: {
				if(is_endian_little_) {
					switch(bytes_per_sample_) {
						case 1: out_format = SND_PCM_FORMAT_S8; return true;
						case 2: out_format = SND_PCM_FORMAT_S16_LE; return true;
						case 3: out_format = SND_PCM_FORMAT_S24_LE; return true;
						case 4: out_format = SND_PCM_FORMAT_S32_LE; return true;
					}
				}
				else {
					switch(bytes_per_sample_) {
						case 1: out_format = SND_PCM_FORMAT_S8; return true;
						case 2: out_format = SND_PCM_FORMAT_S16_BE; return true;
						case 3: out_format = SND_PCM_FORMAT_S24_BE; return true;
						case 4: out_format = SND_PCM_FORMAT_S32_BE; return true;
					}
				}
			}
			break;

			case SampleTypeUnsigned: {
				if(is_endian_little_) {
					switch(bytes_per_sample_) {
						case 1: out_format = SND_PCM_FORMAT_U8; return true;
						case 2: out_format = SND_PCM_FORMAT_U16_LE; return true;
						case 3: out_format = SND_PCM_FORMAT_U24_LE; return true;
						case 4: out_format = SND_PCM_FORMAT_U32_LE; return true;
					}
				}
				else {
					switch(bytes_per_sample_) {
						case 1: out_format = SND_PCM_FORMAT_U8; return true;
						case 2: out_format = SND_PCM_FORMAT_U16_BE; return true;
						case 3: out_format = SND_PCM_FORMAT_U24_BE; return true;
						case 4: out_format = SND_PCM_FORMAT_U32_BE; return true;
					}
				}
			}
			break;

			case SampleTypeFloat: {
				if(is_endian_little_) {
					switch(bytes_per_sample_) {
						case 4: out_format = SND_PCM_FORMAT_FLOAT_LE; return true;
						case 8: out_format = SND_PCM_FORMAT_FLOAT64_LE; return true;
					}
				}
				else {
					switch(bytes_per_sample_) {
						case 4: out_format = SND_PCM_FORMAT_FLOAT_BE; return true;
						case 8: out_format = SND_PCM_FORMAT_FLOAT64_BE; return true;
					}
				}			

			}
			break;

		}
		return false;
	}

	void AlsaPlaybackService::Play(int64_t offset_in_ms) {

		if(!initialized_) {
			throw std::runtime_error("tried to play wav file on an uninitialzed alsa service");
		}

		if(ios_.stopped()) {
			throw std::runtime_error("this instance of alsa playback service has already played in the past. it cannot be reused. create a new instance to play again");
		}

		double position_in_seconds = (double)offset_in_ms / 1000.0;
		curr_position_frames_ = position_in_seconds * (double)frame_rate_;
		curr_position_frames_ = std::min(curr_position_frames_, (int64_t)total_frame_in_file_);
		if(curr_position_frames_ >= 0) {
			sf_count_t seek_res = snd_file_.seek(curr_position_frames_, SEEK_SET);
		}

		logger_->info("start playing file {} from position {} mili-seconds ({} seconds)", file_id_, offset_in_ms, position_in_seconds);
		playing_thread_ = std::thread(&AlsaPlaybackService::PlayingThreadMain, this);
	}

	bool AlsaPlaybackService::Stop() {

		bool was_playing = playing_thread_.joinable() && !ios_.stopped();

		alsa_wait_timer_.cancel();
		ios_.stop();
		if(playing_thread_.joinable()) {
			playing_thread_.join();
		}
		return was_playing;
	}

	void AlsaPlaybackService::PlayingThreadMain() {

		try {
			ios_.post(std::bind(&AlsaPlaybackService::FramesToPcmTransferLoop, this, boost::system::error_code()));
			ios_.run();
			PcmDrop();
		}
		catch(const std::runtime_error &e) {
			logger_->error("play_seq_id: {}. error while playing current wav file. stopped transfering frames to alsa. exception is: {}", play_seq_id_, e.what());
		}
		logger_->info("play_seq_id: {}. handling done", play_seq_id_);
		player_events_callback_->NoSongPlayingStatus(file_id_, play_seq_id_);
		ios_.stop();
	}

	void AlsaPlaybackService::FramesToPcmTransferLoop(boost::system::error_code error_code) {

		// the function might be called from timer, in which case error_code might
		// indicate the timer canceled and we should not invoke the function.
		if(error_code)
			return;

		std::stringstream err_desc;
		int err;

		// calculate how many frames to write
		snd_pcm_sframes_t frames_to_deliver;
		if( (frames_to_deliver = snd_pcm_avail_update(alsa_playback_handle_)) < 0) {
			if(frames_to_deliver == -EPIPE) {
				throw std::runtime_error("an xrun occured");
			}
			else {
				err_desc << "unknown ALSA avail update return value (" << frames_to_deliver << ")";
				throw std::runtime_error(err_desc.str());
			}
		}
		else if(frames_to_deliver == 0) {
			alsa_wait_timer_.expires_from_now(boost::posix_time::millisec(5));
			alsa_wait_timer_.async_wait(std::bind(&AlsaPlaybackService::FramesToPcmTransferLoop, this, std::placeholders::_1));
			return;
		}

		// we want to deliver as many frames as possible.
		// we can put frames_to_deliver number of frames, but the buffer can only hold frames_capacity_in_buffer_ frames
		frames_to_deliver = std::min(frames_to_deliver, frames_capacity_in_buffer_);

		// read the frames from the file. TODO: what if readRaw fails?
		char buffer_for_transfer[TRANSFER_BUFFER_SIZE];
		
		bool start_in_future = (curr_position_frames_ < 0);
		if(!start_in_future) {
			unsigned int bytes_to_deliver = frames_to_deliver * bytes_per_frame_;
			bytes_to_deliver = snd_file_.readRaw(buffer_for_transfer, bytes_to_deliver);
			if(bytes_to_deliver < 0) {
				err_desc << "Failed reading raw frames from snd file. returned: " << sf_error_number(bytes_to_deliver);
				throw std::runtime_error(err_desc.str());				
			}
			if(bytes_to_deliver == 0) {
				logger_->info("play_seq_id: {}. done writing all frames to pcm. waiting for audio device to play remaining frames in the buffer", play_seq_id_);
				ios_.post(std::bind(&AlsaPlaybackService::PcmDrainLoop, this, boost::system::error_code()));
				return;
			}
		}
		else {
			frames_to_deliver = std::min(frames_to_deliver, (snd_pcm_sframes_t)-curr_position_frames_);
			unsigned int bytes_to_deliver = frames_to_deliver * bytes_per_frame_;
			bzero(buffer_for_transfer, bytes_to_deliver);
		}

		int frames_written = snd_pcm_writei(alsa_playback_handle_, buffer_for_transfer, frames_to_deliver);
		if( frames_written < 0) {
			err_desc << "snd_pcm_writei failed (" << snd_strerror(frames_written) << ")";
			throw std::runtime_error(err_desc.str());				
		}

		curr_position_frames_ += frames_written;
		if( (curr_position_frames_ >= 0) && (start_in_future || (frames_written != frames_to_deliver))) {
			logger_->warn("play_seq_id: {}. transfered to alsa less frame then requested. frames_to_deliver: {}, frames_written: {}", play_seq_id_, frames_to_deliver, frames_written);
			snd_file_.seek(curr_position_frames_, SEEK_SET);
		}

		CheckSongStartTime();

		ios_.post(std::bind(&AlsaPlaybackService::FramesToPcmTransferLoop, this, boost::system::error_code()));
	}

	void AlsaPlaybackService::PcmDrainLoop(boost::system::error_code error_code) {

		if(error_code)
			return;

		bool is_currently_playing = IsAlsaStatePlaying();

		if(!is_currently_playing) {
			logger_->info("play_seq_id: {}. playing audio file ended successfully (transfered all frames to pcm and it is empty).", play_seq_id_);
			return;
		}

		CheckSongStartTime();

		alsa_wait_timer_.expires_from_now(boost::posix_time::millisec(5));
		alsa_wait_timer_.async_wait(std::bind(&AlsaPlaybackService::PcmDrainLoop, this, std::placeholders::_1));
	}

	void AlsaPlaybackService::PcmDrop() 
	{
		int err;
		if( (err = snd_pcm_drop(alsa_playback_handle_)) < 0 ) {
			std::stringstream err_desc;
			err_desc << "snd_pcm_drop failed (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}
	}

	void AlsaPlaybackService::CheckSongStartTime() {
		int err;
		snd_pcm_sframes_t delay = 0;

		if( (err = snd_pcm_delay(alsa_playback_handle_, &delay)) < 0) {
			std::stringstream err_desc;
			err_desc << "cannot query current offset in buffer (" << snd_strerror(err) << ")";
			throw std::runtime_error(err_desc.str());
		}	 

		// this is a magic number test to remove end of file wrong reporting
		if(delay < 4096) {
			return;
		}
		int64_t pos_in_frames = curr_position_frames_ - delay;
		int64_t ms_since_audio_file_start = ((pos_in_frames * (int64_t)1000) / (int64_t)frame_rate_);

		struct timeval tv;
		gettimeofday(&tv, NULL);
		// convert sec to ms and usec to ms
		uint64_t curr_time_ms_since_epoch = (uint64_t)(tv.tv_sec) * 1000 + (uint64_t)(tv.tv_usec) / 1000;
		uint64_t audio_file_start_time_ms_since_epoch = (int64_t)curr_time_ms_since_epoch - ms_since_audio_file_start;

		int64_t diff_from_prev = audio_file_start_time_ms_since_epoch - audio_start_time_ms_since_epoch_;
		// there might be small jittering, we don't want to update the value often.
		if(diff_from_prev <= 1 && diff_from_prev >= -1)
			return;

		player_events_callback_->NewSongStatus(file_id_, play_seq_id_, audio_file_start_time_ms_since_epoch, 1.0);

		std::stringstream msg_stream;
		msg_stream << "play_seq_id: " << play_seq_id_ << ". ";
		msg_stream << "calculated a new audio file start time: " << audio_file_start_time_ms_since_epoch << " (ms since epoch). ";
		if(audio_start_time_ms_since_epoch_ > 0) {
			msg_stream << "this is a change since last calculation of " << diff_from_prev << " ms. ";
		}
		msg_stream << "pcm delay in frames as reported by alsa: " << delay << " and position in file is " << 
			ms_since_audio_file_start << " ms. ";
		logger_->info(msg_stream.str());

		audio_start_time_ms_since_epoch_ = audio_file_start_time_ms_since_epoch;
	}

	bool AlsaPlaybackService::IsAlsaStatePlaying() 
	{
		int status = snd_pcm_state(alsa_playback_handle_);
		// the code had SND_PCM_STATE_PREPARED as well.
		// it is removed, to resolve issue of song start playing after end of file.
		// the drain function would not finish since the status is 'SND_PCM_STATE_PREPARED'
		// because no frames were sent to alsa.
		return (status == SND_PCM_STATE_RUNNING); // || (status == SND_PCM_STATE_PREPARED);
	}

    void AlsaPlaybackServiceFactory::Initialize(
            std::shared_ptr<spdlog::logger> logger,
			PlayerEventsIfc *player_events_callback,
            const std::string &audio_device
        )
    {
        logger_ = logger;
		player_events_callback_ = player_events_callback;
        audio_device_ = audio_device;
    }

    IAlsaPlaybackService* AlsaPlaybackServiceFactory::CreateAlsaPlaybackService(
            const std::string &full_file_name, 
            const std::string &file_id,
			uint32_t play_seq_id
        )
    {
        return new AlsaPlaybackService(
            logger_->clone("alsa_playback_service"), // TODO - use file name or id
			player_events_callback_,
            full_file_name,
            file_id,
			audio_device_,
			play_seq_id
        );
    }

}