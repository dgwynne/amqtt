# amqtt - non-blocking event driven MQTT client library

`amqtt` supports the integration of MQTT client functionality into
non-blocking event driven software without knowledge of the underlying
transport (eg, raw sockets or TLS) or how events handling is
implemented (eg, poll(2), libevent, libev, etc).
