#ifndef WAVPLAYERALSA_PLAYER_ACTIONS_IFC_H_
#define WAVPLAYERALSA_PLAYER_ACTIONS_IFC_H_

#include <string>
#include <cstdint>
#include <sstream>
#include <list>

/*
This interface describe the actions that can be performed on the player externally
*/

namespace wavplayeralsa {

	class CurrentSongActionsIfc {
	
	public:

		virtual bool NewSongRequest(
			const std::string &file_id, 
			int64_t start_offset_ms, 
			std::stringstream &out_msg,
			uint32_t *play_seq_id) = 0;

		virtual bool StopPlayRequest(
			std::stringstream &out_msg, 
			uint32_t *play_seq_id) = 0;
	};

	class PlayerFilesActionsIfc {

	public:
		virtual std::list<std::string> QueryFiles() = 0;

	};

}


#endif // WAVPLAYERALSA_PLAYER_ACTIONS_IFC_H_