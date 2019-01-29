#ifndef __SINGLE_FILE_PLAYER_H__
#define __SINGLE_FILE_PLAYER_H__

#include <string>
#include <thread>
#include <mutex>
#include <cstdint>

#include <boost/asio.hpp>
#include <alsa/asoundlib.h>

// forward declerations
class SndfileHandle;
//struct snd_pcm_t;

namespace wavplayeralsa {

	class SingleFilePlayer {

	public:
		enum PlayStatus {
			Playing = 0,
			Stopping = 1,
			Stopped = 2
		};

	public:

		~SingleFilePlayer();

		const std::string &getFileToPlay();
		void initialize(const std::string &path, const std::string &fileName, boost::asio::io_service *ioSerivce);
		void startPlay(uint32_t positionInMs);
		void stop();
		uint32_t getPositionInMs();
		bool isPlaying();

	private:
		void initSndFile();
		void initAlsa();
		bool isAlsaStatePlaying();

	private:
		void playLoopOnThread();


	// state publication stuff
	private:
		// the module is generating status events on the io_service
		boost::asio::io_service *m_ioService;

	private:

		std::string m_fileToPlay;
		std::string m_fullFileName;

	private:
	    enum SampleType {
	    	SampleTypeSigned = 0,
	    	SampleTypeUnsigned = 1,
	    	SampleTypeFloat = 2
	    };
	    unsigned int m_frameRate = 44100;
	    unsigned int m_channels = 2;
	    bool m_isEndianLittle = true; // if false the endian is big :)
	    SampleType m_sampleType = SampleTypeSigned;
	    unsigned int m_sampleChannelSizeBytes = 2;
	    uint64_t m_totalFrames = 0;
	    char *m_rawDataBuffer = NULL;
	    bool m_sndFileInitialized = false;

	private:
		bool GetFormatForAlsa(snd_pcm_format_t &outFormat);
		snd_pcm_t *m_alsaPlaybackHandle;
		uint64_t m_currPositionInFrames = 0;
		std::mutex m_currPositionMutex;
		bool m_alsaInitialized = false;

	private:
		PlayStatus m_playStatus = Stopped;
		std::mutex m_playStatusMutex;

	private:
		std::thread *m_playingThread = NULL;


	};

}


#endif //__SINGLE_FILE_PLAYER_H__