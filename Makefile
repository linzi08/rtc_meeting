# RTC Platform Makefile
SERVICES = signal-gw room-svc scheduler media-gateway

.PHONY: build run-signal run-sfu run-room run-sched clean

build:
	@for s in $(SERVICES); do \
		echo "building $$s..."; \
		go build -o bin/$$s ./services/$$s; \
	done

run-signal: build
	./bin/signal-gw -port 8080 -public web/demo

run-sfu: build
	./bin/media-gateway -port 9090 -worker-bin $(WORKER_BIN) -listen-ip 0.0.0.0 -announced-ip $(ANNOUNCED_IP) -workers 1

run-room: build
	./bin/room-svc -port 8081

run-sched: build
	./bin/scheduler -port 8082

clean:
	rm -rf bin/
