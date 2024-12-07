#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <queue>
#include <algorithm>
#include <unordered_map>
#include "json.hpp"       // For JSON parsing

using json = nlohmann::json;
using namespace std;
int num_clusters = 128;
int embedding_dim = 384;
int batch_size = 65536;

void computeCosineSimilarities(
    const float* hostBatchVectors,
    const float* hostQueryVector,
    float* hostSimilarityScores,
    size_t numVectors,
    size_t vectorDim
);

class IVFIndex 
{
    public:
    IVFIndex
    (
        vector<vector<float>> cluster_embeddings, // 128 clusters - each cluster has x embeddings of size 384 each
        vector<vector<int>> cluster_mappings,
        vector<float> cluster_centroids,
        int n_probe = 8
    ) : cluster_embeddings(cluster_embeddings),
        cluster_mappings(cluster_mappings),
        cluster_centroids(cluster_centroids),
        n_probe(n_probe) {}

    //returns the top k centroids
    vector<pair<float, int>> findSimilar(
    float* flattenedEmbeddings,
    float* query,
    int numEmbeddings,
    int vectorSize,
    int topK,
    int batchSize,
    bool useCuda) 
    {
        if (batchSize == 0) 
        {
            batchSize = numEmbeddings;
        }

        // min heap to keep track of top k vectors
        typedef std::priority_queue<
            pair<float, int>,
            vector<pair<float, int>>,
            greater<pair<float, int>>
        > myheap;
        myheap heap;
        if(!useCuda)
        {
            // compute cosine similarity on CPU
            for (int i = 0; i < numEmbeddings; i++) 
            {
                float score = 0;
                for (int j = 0; j < vectorSize; j++) 
                {
                    score += query[j] * flattenedEmbeddings[i * vectorSize + j];
                }
                heap.push(make_pair(score, i));
                if (heap.size() > topK) 
                {
                    heap.pop();
                }
            }
            vector<pair<float, int>> result;
            myheap tempHeap = heap;
            while (!tempHeap.empty()) 
            {
                result.push_back(tempHeap.top());
                tempHeap.pop();
            }
            std::reverse(result.begin(), result.end());
            return result;
        }
        // loop over chunks in dataset
        for (int i = 0; i < numEmbeddings; i += batchSize) 
        {
            int currentBatchSize = min(batchSize, numEmbeddings - i);

            // get cosine similarity on this batch
            vector<float> scores(currentBatchSize);
            computeCosineSimilarities(
                    flattenedEmbeddings + i * vectorSize,
                    query,
                    scores.data(),
                    currentBatchSize,
                    vectorSize
                );

            // update heap to keep track of top k
            for (int j = 0; j < currentBatchSize; j++) 
            {
                if (heap.size() < topK) 
                {
                    heap.push(make_pair(scores[j], i + j));
                } 
                else if (make_pair(scores[j], i + j) > heap.top()) 
                {
                    heap.pop();
                    heap.push(make_pair(scores[j], i + j));
                }
            }
        }

        vector<pair<float, int>> result;
        myheap tempHeap = heap;
        while (!tempHeap.empty()) 
        {
            result.push_back(tempHeap.top());
            tempHeap.pop();
        }
        std::reverse(result.begin(), result.end());
        return result;
    }

    //Returns top k results from searching top n_probe centroids
    vector<pair<float, int>> search(vector<float>& query, int k, bool use_cuda = false) 
    {
        // Find top centroids
        auto top_centroids = findSimilar(cluster_centroids.data(), query.data(), num_clusters, embedding_dim, n_probe, batch_size, use_cuda);

        // Min-heap to store top k results
        //similarity, index
        auto cmp = [](const pair<float, int>& left, const pair<float, int>& right) 
        {
            return left.first > right.first; 
        };
        priority_queue<pair<float, int>, vector<pair<float, int>>, decltype(cmp)> min_heap(cmp);

        // Iterate over top centroids
        for (const auto& centroid : top_centroids) 
        {
            int cluster = centroid.second;

            // Find similar embeddings in the cluster
            int elements_in_cluster = cluster_embeddings[cluster].size() / embedding_dim;
            auto similarities = findSimilar(cluster_embeddings[cluster].data(), query.data(), elements_in_cluster, embedding_dim, k, batch_size, use_cuda);

            for (const auto& sim : similarities) 
            {
                float score = sim.first;
                int idx = sim.second;
                int mapping_value = cluster_mappings[cluster][idx];

                if (min_heap.size() < k) 
                {
                    min_heap.emplace(score, mapping_value);
                } else if (score > min_heap.top().first) 
                {
                    min_heap.pop();
                    min_heap.emplace(score, mapping_value);
                }
            }
        }

        // Extract results from the heap
        vector<pair<float, int>> results;
        while (!min_heap.empty()) 
        {
            results.push_back(min_heap.top());
            min_heap.pop();
        }
        reverse(results.begin(), results.end()); 
        return results;
    }

    // Static method to load pretrained index
    static IVFIndex from_pretrained(const string& data_dir, int n_probe = 8) 
    {
        // Load cluster mappings from JSON
        ifstream mapping_file(data_dir + "/cluster_mappings_Small_Data.json");
        json cluster_mappings_json;
        mapping_file >> cluster_mappings_json;
        mapping_file.close();

        // Convert JSON to vector of vectors
        vector<vector<int>> cluster_mappings =
            cluster_mappings_json.get<vector<vector<int>>>();

        int n_clusters = cluster_mappings.size();

        // Load cluster embeddings
        vector<vector<float>> cluster_embeddings(n_clusters);

        for (int i = 0; i < n_clusters; ++i) 
        {
            string filename = data_dir + "/cluster_embeddings_" + to_string(i) + "_Small_Data" + ".bin";

            // Read binary file
            ifstream file(filename, ios::binary | ios::ate);
            if (!file.is_open()) 
            {
                throw runtime_error("Failed to open file: " + filename);
            }

            // Get file size and calculate number of rows and columns
            streamsize file_size = file.tellg();
            file.seekg(0, ios::beg);

            int cols = 384; // Set this to the correct embedding dimension
            int rows = file_size / (sizeof(float) * cols);
            if (file_size % (sizeof(float) * cols) != 0) 
            {
                throw runtime_error("File size is not consistent with the expected float dimensions.");
            }

            // Read data into a single buffer
            vector<float> buffer(rows * cols);
            file.read(reinterpret_cast<char*>(buffer.data()), file_size);
            file.close();

            // Convert to vector of vectors
            cluster_embeddings[i].resize(rows * cols);
            cluster_embeddings[i] = buffer;
        }

        // Load cluster centroids
        string centroids_filename = data_dir + "/cluster_centroids_Small_Data.bin";
        ifstream centroids_file(centroids_filename, ios::binary | ios::ate);
        if (!centroids_file.is_open()) 
        {
            throw runtime_error("Failed to open file: " + centroids_filename);
        }

        // Get file size and calculate number of rows and columns
        streamsize centroids_file_size = centroids_file.tellg();
        centroids_file.seekg(0, ios::beg);

        int centroid_cols = 384; 
        int centroid_rows = centroids_file_size / (sizeof(float) * centroid_cols);
        if (centroids_file_size % (sizeof(float) * centroid_cols) != 0) 
        {
            throw runtime_error("Centroids file size is not consistent with the expected float dimensions.");
        }

        // Read data into a single buffer
        vector<float> centroids_buffer(centroid_rows * centroid_cols);
        centroids_file.read(reinterpret_cast<char*>(centroids_buffer.data()), centroids_file_size);
        centroids_file.close();

        // // Convert to vector of vectors
        // vector<vector<float>> cluster_centroids(centroid_rows, vector<float>(centroid_cols));
        // for (size_t r = 0; r < centroid_rows; ++r) 
        // {
        //     for (size_t c = 0; c < centroid_cols; ++c) 
        //     {
        //         cluster_centroids[r][c] = centroids_buffer[r * centroid_cols + c];
        //     }
        // }

        // Create and return IVFIndex instance
        return IVFIndex(cluster_embeddings, cluster_mappings, centroids_buffer, n_probe);
    }

    private:
    // Member variables
    vector<vector<float>> cluster_embeddings; 
    vector<vector<int>> cluster_mappings;
    vector<float> cluster_centroids; 
    int n_probe;

    // Placeholder for similarity_search (to be implemented separately)
    struct SimilaritySearch {
        vector<pair<float, int>> find_similar(
            const vector<vector<float>>& data,
            const vector<float>& query,
            int k,
            bool use_cuda
        ) {
            // Implementation goes here
            return {};
        }
    } similarity_search;
};

int main()
{
    // Load pretrained index
    IVFIndex index = IVFIndex::from_pretrained("/scratch/pvg2018/embeddings_data");

    string filePath = "./queries_data/query1.bin";
    std::ifstream file(filePath, std::ios::binary | std::ios::ate);

    if (!file.is_open()) {
        throw std::runtime_error("Failed to open file: " + filePath);
    }

    // Get the file size
    std::streamsize fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    // Check that the file size is a multiple of the size of float
    if (fileSize % sizeof(float) != 0) {
        throw std::runtime_error("File size is not a multiple of float size.");
    }

    // Create a vector to hold the data
    std::vector<float> query(fileSize / sizeof(float));

    // Read the data into the vector
    if (!file.read(reinterpret_cast<char*>(query.data()), fileSize)) {
        throw std::runtime_error("Failed to read file: " + filePath);
    }

    // Search
    int k = 5;
    vector<pair<float, int>> results = index.search(query, k);

    // Print results
    for (const auto& result : results) {
        cout << result.first << ", " << result.second << endl;
    }
}
