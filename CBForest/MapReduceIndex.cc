//
//  MapReduceIndex.cc
//  CBForest
//
//  Created by Jens Alfke on 5/15/14.
//  Copyright (c) 2014 Couchbase. All rights reserved.
//
//  Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
//  except in compliance with the License. You may obtain a copy of the License at
//    http://www.apache.org/licenses/LICENSE-2.0
//  Unless required by applicable law or agreed to in writing, software distributed under the
//  License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
//  either express or implied. See the License for the specific language governing permissions
//  and limitations under the License.

#include "MapReduceIndex.hh"
#include "Collatable.hh"


namespace forestdb {

    static int64_t kMinFormatVersion = 1;
    static int64_t kCurFormatVersion = 1;

    class emitter : public EmitFn {
    public:
        virtual void operator() (Collatable key, Collatable value);
        std::vector<Collatable> keys;
        std::vector<Collatable> values;
    };

    void emitter::operator() (Collatable key, Collatable value) {
        keys.push_back(key);
        values.push_back(value);
    }


    MapReduceIndex::MapReduceIndex(Database* db, std::string name, KeyStore sourceStore)
    :Index(db, name),
     _sourceDatabase(sourceStore), _map(NULL), _indexType(0),
     _lastSequenceIndexed(0), _lastSequenceChangedAt(0), _stateReadAt(0), _rowCount(0)
    {
        readState();
    }

    void MapReduceIndex::readState() {
        sequence curIndexSeq = KeyStore::lastSequence();
        if (_stateReadAt != curIndexSeq) {
            Collatable stateKey;
            stateKey.addNull();
            Document state = get(stateKey);
            CollatableReader reader(state.body());
            if (reader.peekTag() == CollatableReader::kArray) {
                reader.beginArray();
                _lastSequenceIndexed = reader.readInt();
                _lastSequenceChangedAt = reader.readInt();
                _lastMapVersion = std::string(reader.readString());
                _indexType = (int)reader.readInt();
                _rowCount = (uint64_t)reader.readInt();

                if (reader.peekTag() == CollatableTypes::kEndSequence
                        || reader.readInt() < kMinFormatVersion) {
                    // Obsolete index version
                    deleted();
                    _indexType = 0;
                }
            }
            _stateReadAt = curIndexSeq;
        }
    }

    void MapReduceIndex::saveState(Transaction& t) {
        _lastMapVersion = _mapVersion;

        Collatable stateKey;
        stateKey.addNull();

        Collatable state;
        state << _lastSequenceIndexed << _lastSequenceChangedAt << _lastMapVersion << _indexType
              << _rowCount << kCurFormatVersion;

        _stateReadAt = t(this).set(stateKey, state);
    }

    void MapReduceIndex::deleted() {
        _lastSequenceIndexed = 0;
        _lastSequenceChangedAt = 0;
        _lastMapVersion = "";
        _stateReadAt = 0;
        _rowCount = 0;
    }

    sequence MapReduceIndex::lastSequenceIndexed() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _lastSequenceIndexed;
    }

    sequence MapReduceIndex::lastSequenceChangedAt() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _lastSequenceChangedAt;
    }

    uint64_t MapReduceIndex::rowCount() const {
        const_cast<MapReduceIndex*>(this)->readState();
        return _rowCount;
    }


    void MapReduceIndex::setup(Transaction &t, int indexType, MapFn *map, std::string mapVersion) {
        readState();
        if (indexType != _indexType || mapVersion != _lastMapVersion) {
            _map = map;
            _indexType = indexType;
            _mapVersion = mapVersion;
            if (_lastSequenceIndexed > 0) {
                KeyStore::erase(t);
            }
            _lastSequenceIndexed = _lastSequenceChangedAt = 0;
            _rowCount = 0;
            _stateReadAt = 0;
        }
    }

    void MapReduceIndex::erase(Transaction& t) {
        KeyStore::erase(t);
        _lastSequenceIndexed = _lastSequenceChangedAt = 0;
        _rowCount = 0;
        _stateReadAt = 0;
    }

    bool MapReduceIndex::updateDocInIndex(Transaction& t, const Mappable& mappable) {
        const Document& doc = mappable.document();
        if (doc.sequence() <= _lastSequenceIndexed)
            return false;
        emitter emit;
        if (!doc.deleted())
            (*_map)(mappable, emit); // Call map function!
        _lastSequenceIndexed = doc.sequence();
        if (IndexWriter(this,t).update(doc.key(), doc.sequence(), emit.keys, emit.values, _rowCount)) {
            _lastSequenceChangedAt = _lastSequenceIndexed;
            return true;
        }
        return false;
    }

    
#pragma mark - MAP-REDUCE INDEXER


    MapReduceIndexer::MapReduceIndexer(std::vector<MapReduceIndex*> indexes,
                                       Transaction& transaction)
    :_transaction(transaction),
     _indexes(indexes),
     _triggerIndex(NULL),
     _finished(false)
    { }

    bool MapReduceIndexer::run() {
        KeyStore sourceStore = _indexes[0]->sourceStore();
        sequence latestDbSequence = sourceStore.lastSequence();

        // First find the minimum sequence that not all indexes have indexed yet.
        // Also start a transaction for each index:
        sequence startSequence = latestDbSequence+1;
        for (auto idx = _indexes.begin(); idx != _indexes.end(); ++idx) {
            sequence lastSequence = (*idx)->lastSequenceIndexed();
            if (lastSequence < latestDbSequence) {
                startSequence = std::min(startSequence, lastSequence+1);
            } else if (*idx == _triggerIndex) {
                return false; // The trigger index doesn't need to be updated, so abort
            }
            _lastSequences.push_back(lastSequence);
        }

        if (startSequence > latestDbSequence)
            return false; // no updating needed

        // Enumerate all the documents:
        DocEnumerator::Options options = DocEnumerator::Options::kDefault;
        options.includeDeleted = true;
        for (DocEnumerator e(sourceStore, startSequence, UINT64_MAX, options); e; ++e) {
            addDocument(*e);
        }
        _finished = true;
        return true;
    }

    MapReduceIndexer::~MapReduceIndexer() {
        if (_finished) {
            for (auto i = _indexes.begin(); i != _indexes.end(); ++i)
                (*i)->saveState(_transaction);
        }
    }

    void MapReduceIndexer::addDocument(const Document& doc) {
        Mappable mappable(doc);
        addMappable(mappable);
    }

    void MapReduceIndexer::addMappable(const Mappable& mappable) {
        const size_t n = indexCount();
        for (size_t i = 0; i < n; ++i)
            updateDocInIndex(i, mappable);
    }

}
