# amqtt - non-blocking event driven MQTT client library

`amqtt` supports the integration of MQTT client functionality into
non-blocking event driven software without knowledge of the underlying
transport for the MQTT communication (eg, raw sockets or TLS), or
how event handling is implemented (eg, poll(2), libevent, libev,
etc).
