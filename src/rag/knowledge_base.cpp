#include "rag/knowledge_base.h"
#include <spdlog/spdlog.h>
#include <algorithm>

namespace aegisgate {

static const std::string kPartitionKey = "rag_kb";

KnowledgeBase::KnowledgeBase(Embedder& embedder, VectorStore& store)
    : embedder_(embedder), store_(store) {}

KnowledgeBase::KnowledgeBase(Embedder& embedder, VectorStore& store,
                             KnowledgeBaseConfig config)
    : embedder_(embedder), store_(store), config_(std::move(config)) {}

std::string KnowledgeBase::generateDocId() {
    return "doc_" + std::to_string(++doc_counter_);
}

std::string KnowledgeBase::generateChunkId(const std::string& doc_id, int position) {
    return doc_id + "_chunk_" + std::to_string(position);
}

std::vector<std::string> KnowledgeBase::chunkText(const std::string& text,
                                                   size_t chunk_size,
                                                   size_t overlap) {
    std::vector<std::string> chunks;
    if (text.empty() || chunk_size == 0) return chunks;

    if (text.size() <= chunk_size) {
        chunks.push_back(text);
        return chunks;
    }

    size_t step = (chunk_size > overlap) ? (chunk_size - overlap) : 1;
    for (size_t pos = 0; pos < text.size(); pos += step) {
        size_t end = std::min(pos + chunk_size, text.size());
        chunks.push_back(text.substr(pos, end - pos));
        if (end == text.size()) break;
    }

    return chunks;
}

std::string KnowledgeBase::addDocument(const std::string& content,
                                       const nlohmann::json& metadata) {
    std::unique_lock lock(mutex_);

    if (documents_.size() >= config_.max_documents) {
        spdlog::warn("KnowledgeBase: max documents reached ({})", config_.max_documents);
        return "";
    }

    std::string doc_id = generateDocId();
    auto text_chunks = chunkText(content, config_.chunk_size, config_.chunk_overlap);

    if (text_chunks.size() > config_.max_chunks_per_document) {
        text_chunks.resize(config_.max_chunks_per_document);
    }

    std::vector<DocumentChunk> chunks;
    chunks.reserve(text_chunks.size());

    for (int i = 0; i < static_cast<int>(text_chunks.size()); ++i) {
        std::string chunk_id = generateChunkId(doc_id, i);
        auto embedding = embedder_.embed(text_chunks[i]);

        store_.insert(kPartitionKey, chunk_id, embedding);

        DocumentChunk dc(chunk_id, doc_id, text_chunks[i], i);
        dc.embedding = std::move(embedding);
        chunks.push_back(std::move(dc));
    }

    std::string title = metadata.value("title", doc_id);
    DocumentInfo info(doc_id, title, chunks.size());
    info.metadata = metadata;

    documents_.emplace(doc_id, std::move(info));
    doc_chunks_.emplace(doc_id, std::move(chunks));

    spdlog::debug("KnowledgeBase: added doc={} chunks={}", doc_id, text_chunks.size());
    return doc_id;
}

bool KnowledgeBase::removeDocument(const std::string& doc_id) {
    std::unique_lock lock(mutex_);

    auto chunk_it = doc_chunks_.find(doc_id);
    if (chunk_it == doc_chunks_.end()) return false;

    for (const auto& chunk : chunk_it->second) {
        store_.remove(kPartitionKey, chunk.id);
    }

    doc_chunks_.erase(chunk_it);
    documents_.erase(doc_id);

    spdlog::debug("KnowledgeBase: removed doc={}", doc_id);
    return true;
}

std::vector<RetrievalResult> KnowledgeBase::search(const std::string& query,
                                                    int top_k,
                                                    float min_relevance) const {
    std::shared_lock lock(mutex_);

    auto query_embedding = embedder_.embed(query);
    auto vs_results = store_.search(kPartitionKey, query_embedding,
                                    static_cast<size_t>(top_k), min_relevance);

    std::vector<RetrievalResult> results;
    results.reserve(vs_results.size());

    for (const auto& vsr : vs_results) {
        for (const auto& [did, chunks] : doc_chunks_) {
            for (const auto& chunk : chunks) {
                if (chunk.id == vsr.id) {
                    RetrievalResult rr(chunk.id, chunk.document_id,
                                       chunk.content, vsr.score);
                    rr.position = chunk.position;

                    auto doc_it = documents_.find(chunk.document_id);
                    if (doc_it != documents_.end()) {
                        rr.metadata = doc_it->second.metadata;
                    }

                    results.push_back(std::move(rr));
                    goto next_result;
                }
            }
        }
        next_result:;
    }

    return results;
}

std::vector<DocumentInfo> KnowledgeBase::listDocuments() const {
    std::shared_lock lock(mutex_);
    std::vector<DocumentInfo> result;
    result.reserve(documents_.size());
    for (const auto& [id, info] : documents_) {
        result.push_back(info);
    }
    return result;
}

std::optional<DocumentInfo> KnowledgeBase::getDocument(const std::string& doc_id) const {
    std::shared_lock lock(mutex_);
    auto it = documents_.find(doc_id);
    if (it == documents_.end()) return std::nullopt;
    return it->second;
}

size_t KnowledgeBase::documentCount() const {
    std::shared_lock lock(mutex_);
    return documents_.size();
}

size_t KnowledgeBase::chunkCount() const {
    std::shared_lock lock(mutex_);
    size_t total = 0;
    for (const auto& [id, chunks] : doc_chunks_) {
        total += chunks.size();
    }
    return total;
}

void KnowledgeBase::clear() {
    std::unique_lock lock(mutex_);
    for (const auto& [doc_id, chunks] : doc_chunks_) {
        for (const auto& chunk : chunks) {
            store_.remove(kPartitionKey, chunk.id);
        }
    }
    documents_.clear();
    doc_chunks_.clear();
    doc_counter_ = 0;
    spdlog::debug("KnowledgeBase: cleared all documents");
}

} // namespace aegisgate
