APP := cli-netconf

.PHONY: build run clean docker-build test

build:
	go build -o bin/$(APP) .

run: build
	./bin/$(APP)

clean:
	rm -rf bin/

docker-build:
	docker build -t $(APP) .

test:
	go test -v -timeout 300s ./test/

generate-host-key:
	ssh-keygen -t ed25519 -f host_key -N ""
