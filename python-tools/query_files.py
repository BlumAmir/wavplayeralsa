import argparse
import http.client
import json

parser = argparse.ArgumentParser(description='query which files are availablie for play')

parser.add_argument('--ip_address', action="store", dest="ip_address", default="127.0.0.1", type=str, help="ip or host name of the http server (player's host)")
parser.add_argument('--port', action="store", dest="port", default=8080, type=int, help="port of the http server (player's host)")
results = parser.parse_args()

connection = http.client.HTTPConnection(results.ip_address, results.port)
connection.request("GET", "/api/available-files")
response = connection.getresponse()
print("Status: {} and reason: {}".format(response.status, response.reason))
print(response.read())

connection.close()

