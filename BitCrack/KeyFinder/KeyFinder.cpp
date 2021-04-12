#include <fstream>
#include <iostream>

#include "KeyFinder.h"
#include "../Util/util.h"
#include "../AddrUtil/AddressUtil.h"

#include "../Logger/Logger.h"


void KeyFinder::defaultResultCallback(KeySearchResult result)
{
	// Do nothing
}

void KeyFinder::defaultStatusCallback(KeySearchStatus status)
{
	// Do nothing
}

KeyFinder::KeyFinder(const secp256k1::uint256& startKey, const secp256k1::uint256& endKey, int compression,
	KeySearchDevice* device, const secp256k1::uint256& stride, bool randomStride, bool continueAfterEnd, uint32_t randomSrtrideBits)
{
	_total = 0;
	_statusInterval = 1000;
	_device = device;

	_compression = compression;

	_startKey = startKey;

	_endKey = endKey;

	_statusCallback = NULL;

	_resultCallback = NULL;

	_iterCount = 0;

	_stride = stride;

	_randomStride = randomStride;
	_continueAfterEnd = continueAfterEnd;
	_randomSrtrideBits = randomSrtrideBits;
	_rStrideCount = 0;
	_rStrideHistory.clear();

	if (_randomStride)
		_stride = secp256k1::getRandom32(_randomSrtrideBits, _rStrideHistory);
}

KeyFinder::~KeyFinder()
{
}

void KeyFinder::setTargets(std::vector<std::string>& targets)
{
	if (targets.size() == 0) {
		throw KeySearchException("Requires at least 1 target");
	}

	_targets.clear();

	// Convert each address from base58 encoded form to a 160-bit integer
	for (unsigned int i = 0; i < targets.size(); i++) {

		if (!Address::verifyAddress(targets[i])) {
			throw KeySearchException("Invalid address '" + targets[i] + "'");
		}

		KeySearchTarget t;

		Base58::toHash160(targets[i], t.value);

		_targets.insert(t);
	}

	_device->setTargets(_targets);
}

void KeyFinder::setTargets(std::string targetsFile)
{
	std::ifstream inFile(targetsFile.c_str());

	if (!inFile.is_open()) {
		Logger::log(LogLevel::Error, "Unable to open '" + targetsFile + "'");
		throw KeySearchException();
	}

	_targets.clear();

	std::string line;
	Logger::log(LogLevel::Info, "Loading addresses from '" + targetsFile + "'");
	while (std::getline(inFile, line)) {
		util::removeNewline(line);
		line = util::trim(line);

		if (line.length() > 0) {
			if (!Address::verifyAddress(line)) {
				Logger::log(LogLevel::Error, "Invalid address '" + line + "'");
				throw KeySearchException();
			}

			KeySearchTarget t;

			Base58::toHash160(line, t.value);

			_targets.insert(t);
		}
	}
	Logger::log(LogLevel::Info, util::formatThousands(_targets.size()) + " addresses loaded ("
		+ util::format("%.1f", (double)(sizeof(KeySearchTarget) * _targets.size()) / (double)(1024 * 1024)) + "MB)");

	_device->setTargets(_targets);
}


void KeyFinder::setResultCallback(void(*callback)(KeySearchResult))
{
	_resultCallback = callback;
}

void KeyFinder::setStatusCallback(void(*callback)(KeySearchStatus))
{
	_statusCallback = callback;
}

void KeyFinder::setStatusInterval(uint64_t interval)
{
	_statusInterval = interval;
}

void KeyFinder::setTargetsOnDevice()
{
	// Set the target in constant memory
	std::vector<struct hash160> targets;

	for (std::set<KeySearchTarget>::iterator i = _targets.begin(); i != _targets.end(); ++i) {
		targets.push_back(hash160((*i).value));
	}

	_device->setTargets(_targets);
}

void KeyFinder::init()
{
	Logger::log(LogLevel::Info, "Initializing " + _device->getDeviceName());

	_device->init(_startKey, _compression, _stride);
}


void KeyFinder::stop()
{
	_running = false;
}

void KeyFinder::removeTargetFromList(const unsigned int hash[5])
{
	KeySearchTarget t(hash);

	_targets.erase(t);
}

bool KeyFinder::isTargetInList(const unsigned int hash[5])
{
	KeySearchTarget t(hash);
	return _targets.find(t) != _targets.end();
}


void KeyFinder::run()
{
	uint64_t pointsPerIteration = _device->keysPerStep();

	_running = true;

	util::Timer timer;

	timer.start();

	uint64_t prevIterCount = 0;

	_totalTime = 0;

	while (_running) {

		_device->doStep();
		_iterCount++;

		// Update status
		uint64_t t = timer.getTime();

		if (t >= _statusInterval) {

			KeySearchStatus info;

			uint64_t count = (_iterCount - prevIterCount) * pointsPerIteration;

			_total += count;

			double seconds = (double)t / 1000.0;

			info.speed = (double)((double)count / seconds) / 1000000.0;

			info.total = _total;

			info.totalTime = _totalTime;

			uint64_t freeMem = 0;

			uint64_t totalMem = 0;

			_device->getMemoryInfo(freeMem, totalMem);

			info.freeMemory = freeMem;
			info.deviceMemory = totalMem;
			info.deviceName = _device->getDeviceName();
			info.targets = _targets.size();
			info.nextKey = getNextKey();
			info.stride = _stride;
			info.rStrideCount = _rStrideCount;

			_statusCallback(info);

			timer.start();
			prevIterCount = _iterCount;
			_totalTime += t;
		}

		std::vector<KeySearchResult> results;

		if (_device->getResults(results) > 0) {

			for (unsigned int i = 0; i < results.size(); i++) {

				KeySearchResult info;
				info.privateKey = results[i].privateKey;
				info.publicKey = results[i].publicKey;
				info.compressed = results[i].compressed;
				info.address = Address::fromPublicKey(results[i].publicKey, results[i].compressed);

				_resultCallback(info);
			}

			// Remove the hashes that were found
			for (unsigned int i = 0; i < results.size(); i++) {
				removeTargetFromList(results[i].hash);
			}
		}

		// Stop if there are no keys left
		if (_targets.size() == 0) {
			printf("\n");
			Logger::log(LogLevel::Info, "No targets remaining");
			_running = false;
		}

		// Stop if we searched the entire range
		if (_device->getNextKey().cmp(_endKey) >= 0 || _device->getNextKey().cmp(_startKey) < 0) {
			//printf("\n");

			if (_continueAfterEnd && _randomStride) {
				//Logger::log(LogLevel::Info, "Continuing after end...");
				reSetupEverything();

			}
			else {
				Logger::log(LogLevel::Info, "Reached end of keyspace");
				_running = false;
			}
		}
	}
}

secp256k1::uint256 KeyFinder::getNextKey()
{
	return _device->getNextKey();
}

uint64_t KeyFinder::LZC(uint64_t x)
{
	uint64_t c = 0;
	if (x == 0)
		return 64ULL;
	for (int i = 63; i >= 0; i--) {
		if ((1ULL & (uint64_t)x >> (uint64_t)i) == 0)
			c++;
		else
			return c;
	}
	return c;
}

uint64_t KeyFinder::TZC(uint64_t x)
{
	uint64_t c = 0;
	if (x == 0)
		return 64ULL;
	for (int i = 0; i <= 63; i++) {
		if ((1ULL & (uint64_t)x >> (uint64_t)i) == 0)
			c++;
		else
			return c;
	}
	return c;
}

void KeyFinder::reSetupEverything()
{
	_stride = secp256k1::getRandom32(_randomSrtrideBits, _rStrideHistory);
	//Logger::log(LogLevel::Info, "Counting by: " + _stride.toString() + " (" + std::to_string(_stride.getBitRange()) + " bit)");
	_device->updateStride(_stride);
	_rStrideCount++;
}
