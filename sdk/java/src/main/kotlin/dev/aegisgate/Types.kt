package dev.aegisgate

import com.google.gson.annotations.SerializedName

data class ChatMessage(
    val role: String,
    val content: String,
)

data class ChatCompletionRequest(
    val model: String,
    val messages: List<ChatMessage>,
    val temperature: Double? = null,
    @SerializedName("max_tokens")
    val maxTokens: Int? = null,
    val stream: Boolean = false,
)

data class ChatCompletionResponse(
    val id: String,
    @SerializedName("object")
    val objectType: String,
    val model: String,
    val choices: List<ChatChoice>,
    val usage: TokenUsage? = null,
) {
    val content: String
        get() = choices.firstOrNull()?.message?.content ?: ""
}

data class ChatChoice(
    val index: Int,
    val message: ChatChoiceMessage,
    @SerializedName("finish_reason")
    val finishReason: String?,
)

data class ChatChoiceMessage(
    val role: String,
    val content: String,
)

data class TokenUsage(
    @SerializedName("prompt_tokens")
    val promptTokens: Int = 0,
    @SerializedName("completion_tokens")
    val completionTokens: Int = 0,
    @SerializedName("total_tokens")
    val totalTokens: Int = 0,
)

data class ChatCompletionChunk(
    val id: String,
    @SerializedName("object")
    val objectType: String,
    val model: String,
    val choices: List<ChunkChoice>,
)

data class ChunkChoice(
    val index: Int,
    val delta: ChunkDelta,
    @SerializedName("finish_reason")
    val finishReason: String? = null,
)

data class ChunkDelta(
    val role: String? = null,
    val content: String? = null,
)

data class ModelInfo(
    val id: String,
    @SerializedName("object")
    val objectType: String = "model",
    @SerializedName("owned_by")
    val ownedBy: String = "",
)

data class ModelListResponse(
    @SerializedName("object")
    val objectType: String = "list",
    val data: List<ModelInfo> = emptyList(),
)

data class HealthResponse(
    val status: String,
    val version: String,
)
