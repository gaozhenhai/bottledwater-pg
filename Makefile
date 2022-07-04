DOCKER_TAG = dev

.PHONY: all install clean

all:
	$(MAKE) -C ext all
	$(MAKE) -C client all
	$(MAKE) -C kafka all

install:
	$(MAKE) -C ext install

clean:
	$(MAKE) -C ext clean
	$(MAKE) -C client clean
	$(MAKE) -C kafka clean

test-bundle: Gemfile.lock
	bundle install

spec/functional/type_specs.rb: spec/bin/generate_type_specs.rb test-bundle docker-compose
	bundle exec ruby -Ispec $< >$@

test: spec/functional/type_specs.rb
	bundle exec rspec --order random

docker: docker-client docker-postgres docker-postgres94

docker-compose: docker
	docker-compose build