#ifndef WAVPLAYERALSA_HTTP_API_H_
#define WAVPLAYERALSA_HTTP_API_H_

#include <cstdint>

#include <boost/asio.hpp>

#include "simple-web-server/server_http.hpp"
#include "spdlog/spdlog.h"
#include "nlohmann/json_fwd.hpp"

#include "player_actions_ifc.h"


using HttpServer = SimpleWeb::Server<SimpleWeb::HTTP>;

namespace wavplayeralsa {

	class HttpApi {

	public:
		void Initialize(
			std::shared_ptr<spdlog::logger> logger, 
			const std::string &player_uuid,
			boost::asio::io_service *io_service, 
			CurrentSongActionsIfc *current_song_action_callback, 
			PlayerFilesActionsIfc *player_files_action_callback, 
			uint16_t http_listen_port);

	private:
		void OnGetAvailableFiles(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request);
		void OnPutCurrentSong(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request);
		void OnWebGet(std::shared_ptr<HttpServer::Response> response, std::shared_ptr<HttpServer::Request> request);
		void OnServerError(std::shared_ptr<HttpServer::Request> /*request*/, const SimpleWeb::error_code & ec);

	private:
		void WriteResponseBadRequest(std::shared_ptr<HttpServer::Response> response, const std::stringstream &err_stream);
		void WriteResponseSuccess(std::shared_ptr<HttpServer::Response> response, const std::stringstream &body_stream);
		void WriteJsonResponseBadRequest(std::shared_ptr<HttpServer::Response> response, const nlohmann::json &body_json);
		void WriteJsonResponseSuccess(std::shared_ptr<HttpServer::Response> response, const nlohmann::json &body_json);

	private:
		// outside configurartion
		CurrentSongActionsIfc *current_song_action_callback_;
		PlayerFilesActionsIfc *player_files_action_callback_;
		std::shared_ptr<spdlog::logger> logger_;
        std::string player_uuid_;

	private:
		// class private members
		HttpServer server_;

	};

	
}

#endif // WAVPLAYERALSA_HTTP_API_H_