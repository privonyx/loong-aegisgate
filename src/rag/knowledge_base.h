#pragma once
#include "cache/embedder.h"
#include "cache/vector_store.h"
#include <string>
#include <vector>
#include <optional>
#include <unordered_map>
#include <shared_mutex>
#include <nlohmann/json.hpp>

namespace aegisgate {

struct DocumentChunk {
    std::string id;
    std::string document_id;
    std::string content;
    std::vector<float> embedding;
    int position = 0;

    DocumentChunk() = default;
    DocumentChunk(std::string id_in, std::string doc_id_in, std::string content_in, int pos_in)
        : id(std::move(id_in)), document_id(std::move(doc_id_in)),
          content(std::move(content_in)), position(pos_in) {}
};

struct DocumentInfo {
    std::string id;
    std::string title;
    nlohmann::json metadata;
    size_t chunk_count = 0;
    std::chrono::steady_clock::time_point created_at;

    DocumentInfo() = default;
    DocumentInfo(std::string id_in, std::string title_in, size_t chunks)
        : id(std::move(id_in)), title(std::move(title_in)), chunk_count(chunks),
          created_at(std::chrono::steady_clock::now()) {}
};

struct RetrievalResult {
    std::string chunk_id;
    std::string document_id;
    std::string content;
    float relevance = 0.0f;
    int position = 0;
    nlohmann::json metadata;

    RetrievalResult() = default;
    RetrievalResult(std::string cid_in, std::string did_in, std::string cont_in, float rel_in)
        : chunk_id(std::move(cid_in)), document_id(std::move(did_in)),
          content(std::move(cont_in)), relevance(rel_in) {}
};

struct KnowledgeBaseConfig {
    size_t chunk_size = 512;
    size_t chunk_overlap = 64;
    size_t max_documents = 10000;
    size_t max_chunks_per_document = 1000;
};

class KnowledgeBase {
public:
    KnowledgeBase(Embedder& embedder, VectorStore& store);
    explicit KnowledgeBase(Embedder& embedder, VectorStore& store,
                           KnowledgeBaseConfig config);

    std::string addDocument(const std::string& content,
                            const nlohmann::json& metadata = nlohmann::json::object());
    bool removeDocument(const std::string& doc_id);
    std::vector<RetrievalResult> search(const std::string& query,
                                        int top_k = 3,
                                        float min_relevance = 0.0f) const;
    std::vector<DocumentInfo> listDocuments() const;
    std::optional<DocumentInfo> getDocument(const std::string& doc_id) const;
    size_t documentCount() const;
    size_t chunkCount() const;
    void clear();

    static std::vector<std::string> chunkText(const std::string& text,
                                               size_t chunk_size = 512,
                                               size_t overlap = 64);

private:
    std::string generateDocId();
    std::string generateChunkId(const std::string& doc_id, int position);

    Embedder& embedder_;
    VectorStore& store_;
    KnowledgeBaseConfig config_;

    std::unordered_map<std::string, DocumentInfo> documents_;
    std::unordered_map<std::string, std::vector<DocumentChunk>> doc_chunks_;
    size_t doc_counter_ = 0;
    mutable std::shared_mutex mutex_;
};

} // namespace aegisgate
