/*
 * Copyright (C) 2019 Zilliqa
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <boost/lexical_cast.hpp>

#include "Account.h"
#include "common/Messages.h"
#include "depends/common/CommonIO.h"
#include "depends/common/FixedHash.h"
#include "depends/common/RLP.h"
#include "libCrypto/Sha2.h"
#include "libMessage/Messenger.h"
#include "libUtils/DataConversion.h"
#include "libUtils/JsonUtils.h"
#include "libUtils/Logger.h"
#include "libUtils/SafeMath.h"

using namespace std;
using namespace boost::multiprecision;
using namespace dev;

using namespace Contract;

Account::Account() {}

Account::Account(const bytes& src, unsigned int offset) {
  if (!Deserialize(src, offset)) {
    LOG_GENERAL(WARNING, "We failed to init Account.");
  }
}

Account::Account(const uint128_t& balance, const uint64_t& nonce)
    : m_balance(balance),
      m_nonce(nonce),
      m_storageRoot(h256()),
      m_codeHash(h256()) {}

bool Account::isContract() const { return m_codeHash != dev::h256(); }

void Account::InitStorage() {
  if (HASHMAP_CONTRACT_STATE_DB) {
    return;
  }

  // LOG_MARKER();
  m_storage = AccountTrieDB<dev::h256, OverlayDB>(
      &(ContractStorage::GetContractStorage().GetStateDB()));
  m_storage.init();
  if (m_storageRoot != h256()) {
    m_storage.setRoot(m_storageRoot);
    m_prevRoot = m_storageRoot;
  }
}

bool Account::InitContract(const bytes& initData, const Address& addr) {
  m_address = addr;
  SetInitData(initData);
  if (!InitContract()) {
    LOG_GENERAL(WARNING, "Account " << addr.hex() << " InitContract failed");
    return false;
  }
  return true;
}

bool Account::InitContract() {
  // LOG_MARKER();
  if (!PrepareInitDataJson(GetInitData(), m_initValJson)) {
    LOG_GENERAL(WARNING, "PrepareInitDataJson failed");
    return false;
  }

  bool hasScillaVersion = false;
  std::vector<StateEntry> state_entries;

  for (auto& v : m_initValJson) {
    if (!v.isMember("vname") || !v.isMember("type") || !v.isMember("value")) {
      LOG_GENERAL(WARNING,
                  "This variable in initialization of contract is corrupted");
      return false;
    }

    string vname = v["vname"].asString();
    string type = v["type"].asString();

    if (!hasScillaVersion && vname == "_scilla_version" && type == "Uint32") {
      try {
        m_scillaVersion = boost::lexical_cast<uint32_t>(v["value"].asString());
      } catch (...) {
        LOG_GENERAL(WARNING, "_scilla_version is not a number");
        return false;
      }

      hasScillaVersion = true;
    }

    Json::StreamWriterBuilder writeBuilder;
    std::unique_ptr<Json::StreamWriter> writer(writeBuilder.newStreamWriter());
    ostringstream oss;
    writer->write(v["value"], &oss);
    string value = oss.str();

    if (!HASHMAP_CONTRACT_STATE_DB) {
      SetStorage(vname, type, value, false);
    }
    state_entries.push_back(std::make_tuple(vname, false, type, value));
  }

  if (HASHMAP_CONTRACT_STATE_DB) {
    return ContractStorage::GetContractStorage().PutContractState(
        m_address, state_entries, m_storageRoot);
  }

  if (!hasScillaVersion) {
    LOG_GENERAL(WARNING, "No _scilla_version indicated");
    return false;
  }

  return true;
}

void Account::SetCreateBlockNum(const uint64_t& blockNum) {
  m_createBlockNum = blockNum;
}

const uint64_t& Account::GetCreateBlockNum() const { return m_createBlockNum; }

const uint32_t& Account::GetScillaVersion() const { return m_scillaVersion; }

bool Account::Serialize(bytes& dst, unsigned int offset) const {
  if (!Messenger::SetAccount(dst, offset, *this)) {
    LOG_GENERAL(WARNING, "Messenger::SetAccount failed.");
    return false;
  }

  return true;
}

bool Account::Deserialize(const bytes& src, unsigned int offset) {
  LOG_MARKER();

  if (!Messenger::GetAccount(src, offset, *this)) {
    LOG_GENERAL(WARNING, "Messenger::GetAccount failed.");
    return false;
  }

  return true;
}

bool Account::IncreaseBalance(const uint128_t& delta) {
  return SafeMath<uint128_t>::add(m_balance, delta, m_balance);
}

bool Account::DecreaseBalance(const uint128_t& delta) {
  if (m_balance < delta) {
    return false;
  }

  return SafeMath<uint128_t>::sub(m_balance, delta, m_balance);
}

bool Account::ChangeBalance(const int256_t& delta) {
  return (delta >= 0) ? IncreaseBalance(uint128_t(delta))
                      : DecreaseBalance(uint128_t(-delta));
}

void Account::SetBalance(const uint128_t& balance) { m_balance = balance; }

const uint128_t& Account::GetBalance() const { return m_balance; }

bool Account::IncreaseNonce() {
  ++m_nonce;
  return true;
}

bool Account::IncreaseNonceBy(const uint64_t& nonceDelta) {
  m_nonce += nonceDelta;
  return true;
}

void Account::SetNonce(const uint64_t& nonce) { m_nonce = nonce; }

const uint64_t& Account::GetNonce() const { return m_nonce; }

void Account::SetStorageRoot(const h256& root) {
  if (!isContract()) {
    return;
  }

  m_storageRoot = root;

  if (m_storageRoot == h256()) {
    return;
  }

  if (!HASHMAP_CONTRACT_STATE_DB) {
    m_storage.setRoot(m_storageRoot);
  }

  m_prevRoot = m_storageRoot;
}

const dev::h256& Account::GetStorageRoot() const { return m_storageRoot; }

void Account::SetStorage(string k, string type, string v, bool is_mutable) {
  if (!isContract()) {
    return;
  }

  if (HASHMAP_CONTRACT_STATE_DB) {
    return;
  }

  RLPStream rlpStream(4);
  rlpStream << k << (is_mutable ? "True" : "False") << type << v;

  m_storage.insert(GetKeyHash(k), rlpStream.out());

  m_storageRoot = m_storage.root();
}

bool Account::SetStorage(const vector<StateEntry>& state_entries) {
  return ContractStorage::GetContractStorage().PutContractState(
      m_address, state_entries, m_storageRoot);
}

void Account::SetStorage(const h256& k_hash, const string& rlpStr) {
  if (!isContract()) {
    LOG_GENERAL(WARNING, "Not contract account, why call Account::SetStorage!");
    return;
  }

  if (HASHMAP_CONTRACT_STATE_DB) {
    return;
  }

  m_storage.insert(k_hash, rlpStr);
  m_storageRoot = m_storage.root();
}

bool Account::SetStorage(const Address& addr,
                         const vector<pair<dev::h256, bytes>>& entries) {
  if (!ContractStorage::GetContractStorage().PutContractState(addr, entries,
                                                              m_storageRoot)) {
    LOG_GENERAL(WARNING, "PutContractState failed");
    return false;
  }
  return true;
}

string Account::GetRawStorage(const h256& k_hash) const {
  if (!isContract()) {
    // LOG_GENERAL(WARNING,
    //             "Not contract account, why call Account::GetRawStorage!");
    return "";
  }

  if (HASHMAP_CONTRACT_STATE_DB) {
    return ContractStorage::GetContractStorage().GetContractStateData(k_hash);
  }

  return m_storage.at(k_hash);
}

bool Account::PrepareInitDataJson(const bytes& initData, Json::Value& root) {
  if (initData.empty()) {
    LOG_GENERAL(WARNING, "Init data for the contract is empty");
    return false;
  }
  Json::CharReaderBuilder builder;
  unique_ptr<Json::CharReader> reader(builder.newCharReader());
  string dataStr(initData.begin(), initData.end());
  string errors;
  if (!reader->parse(dataStr.c_str(), dataStr.c_str() + dataStr.size(), &root,
                     &errors)) {
    LOG_GENERAL(WARNING,
                "Failed to parse initialization contract json: " << errors);
    return false;
  }

  // Append createBlockNum
  {
    Json::Value createBlockNumObj;
    createBlockNumObj["vname"] = "_creation_block";
    createBlockNumObj["type"] = "BNum";
    createBlockNumObj["value"] = to_string(GetCreateBlockNum());
    root.append(createBlockNumObj);
  }

  // Append _this_address
  {
    Json::Value thisAddressObj;
    thisAddressObj["vname"] = "_this_address";
    thisAddressObj["type"] = "ByStr20";
    thisAddressObj["value"] = "0x" + m_address.hex();
    root.append(thisAddressObj);
  }

  return true;
}

Json::Value Account::GetInitJson(bool record) {
  if (m_initValJson.empty()) {
    Json::Value root;
    if (!PrepareInitDataJson(GetInitData(), root)) {
      LOG_GENERAL(WARNING, "PrepareInitDataJson failed");
      root = Json::arrayValue;
    } else if (record) {
      m_initValJson = root;
    }
    return root;
  } else {
    return m_initValJson;
  }
}

void Account::SetInitData(const bytes& initData) { m_initData = initData; }

const bytes Account::GetInitData() const {
  if (m_initData.empty()) {
    return ContractStorage::GetContractStorage().GetContractInitData(m_address);
  } else {
    return m_initData;
  }
}

void Account::CleanInitData() {
  m_initData.clear();
  m_initValJson.clear();
}

vector<h256> Account::GetStorageKeyHashes() const {
  if (HASHMAP_CONTRACT_STATE_DB) {
    return ContractStorage::GetContractStorage().GetContractStateIndexes(
        m_address);
  }

  vector<h256> keyHashes;
  for (auto const& i : m_storage) {
    keyHashes.emplace_back(i.first);
  }
  return keyHashes;
}

Json::Value Account::GetStorageJson() const {
  if (!isContract()) {
    LOG_GENERAL(WARNING,
                "Not contract account, why call Account::GetStorageJson!");
    return Json::arrayValue;
  }

  Json::Value root;

  if (HASHMAP_CONTRACT_STATE_DB) {
    root =
        ContractStorage::GetContractStorage().GetContractStateJson(m_address);
  } else {
    for (auto const& i : m_storage) {
      dev::RLP rlp(i.second);
      string tVname = rlp[0].toString();
      string tMutable = rlp[1].toString();
      string tType = rlp[2].toString();
      string tValue = rlp[3].toString();
      // LOG_GENERAL(INFO,
      //             "\nvname: " << tVname << " \nmutable: " << tMutable
      //                         << " \ntype: " << tType
      //                         << " \nvalue: " << tValue);
      if (tMutable == "False") {
        continue;
      }

      Json::Value item;
      item["vname"] = tVname;
      item["type"] = tType;
      if (tValue[0] == '[' || tValue[0] == '{') {
        Json::CharReaderBuilder builder;
        unique_ptr<Json::CharReader> reader(builder.newCharReader());
        Json::Value obj;
        string errors;
        if (!reader->parse(tValue.c_str(), tValue.c_str() + tValue.size(), &obj,
                           &errors)) {
          LOG_GENERAL(WARNING,
                      "The json object cannot be extracted from Storage: "
                          << tValue << endl
                          << "Error: " << errors);
          continue;
        }
        item["value"] = obj;
      } else {
        item["value"] = tValue;
      }
      root.append(item);
    }
  }

  Json::Value balance;
  balance["vname"] = "_balance";
  balance["type"] = "Uint128";
  balance["value"] = GetBalance().convert_to<string>();
  root.append(balance);

  // LOG_GENERAL(INFO, "States: " << root);

  return root;
}

void Account::Commit() { m_prevRoot = m_storageRoot; }

void Account::RollBack() {
  if (!isContract()) {
    LOG_GENERAL(WARNING, "Not a contract, why call Account::RollBack");
    return;
  }
  m_storageRoot = m_prevRoot;

  if (!HASHMAP_CONTRACT_STATE_DB) {
    if (m_storageRoot != h256()) {
      m_storage.setRoot(m_storageRoot);
    } else {
      m_storage.init();
    }
  }
}

Address Account::GetAddressFromPublicKey(const PubKey& pubKey) {
  Address address;

  bytes vec;
  pubKey.Serialize(vec, 0);
  SHA2<HASH_TYPE::HASH_VARIANT_256> sha2;
  sha2.Update(vec);

  const bytes& output = sha2.Finalize();

  if (output.size() != 32) {
    LOG_GENERAL(WARNING, "assertion failed (" << __FILE__ << ":" << __LINE__
                                              << ": " << __FUNCTION__ << ")");
  }

  copy(output.end() - ACC_ADDR_SIZE, output.end(), address.asArray().begin());

  return address;
}

Address Account::GetAddressForContract(const Address& sender,
                                       const uint64_t& nonce) {
  Address address;

  SHA2<HASH_TYPE::HASH_VARIANT_256> sha2;
  bytes conBytes;
  copy(sender.asArray().begin(), sender.asArray().end(),
       back_inserter(conBytes));
  SetNumber<uint64_t>(conBytes, conBytes.size(), nonce, sizeof(uint64_t));
  sha2.Update(conBytes);

  const bytes& output = sha2.Finalize();

  if (output.size() != 32) {
    LOG_GENERAL(WARNING, "assertion failed (" << __FILE__ << ":" << __LINE__
                                              << ": " << __FUNCTION__ << ")");
  }

  copy(output.end() - ACC_ADDR_SIZE, output.end(), address.asArray().begin());

  return address;
}

void Account::SetCode(const bytes& code) {
  // LOG_MARKER();

  if (code.size() == 0) {
    LOG_GENERAL(WARNING, "Code for this contract is empty");
    return;
  }

  m_codeCache = code;
  SHA2<HASH_TYPE::HASH_VARIANT_256> sha2;
  sha2.Update(code);
  m_codeHash = dev::h256(sha2.Finalize());
  // LOG_GENERAL(INFO, "m_codeHash: " << m_codeHash);

  InitStorage();
}

const bytes Account::GetCode() const {
  if (m_codeCache.empty()) {
    return ContractStorage::GetContractStorage().GetContractCode(m_address);
  } else {
    return m_codeCache;
  }
}

void Account::CleanCodeCache() { m_codeCache.clear(); }

const dev::h256& Account::GetCodeHash() const { return m_codeHash; }

const h256 Account::GetKeyHash(const string& key) const {
  SHA2<HASH_TYPE::HASH_VARIANT_256> sha2;
  sha2.Update(DataConversion::StringToCharArray(key));
  return h256(sha2.Finalize());
}