#include "pch.h"
#include "aiInternal.h"
#include "aiContext.h"
#include "aiObject.h"
#include "aiSchema.h"
#include "aiPolyMesh.h"
#include <unordered_map>
#include "../Foundation/aiMisc.h"


#define MAX_VERTEX_SPLIT_COUNT_16 65000
#define MAX_VERTEX_SPLIT_COUNT_32 2000000000

static inline int CalculateTriangulatedIndexCount(Abc::Int32ArraySample &counts)
{
    int r = 0;
    size_t n = counts.size();
    for (size_t fi = 0; fi < n; ++fi)
    {
        int ngon = counts[fi];
        r += (ngon - 2) * 3;
    }
    return r;
}

Topology::Topology()
{
}

void Topology::clear()
{
    DebugLog("Topology::clear()");
    m_faceIndices.reset();
    m_vertexCountPerFace.reset();

    m_tangentIndices.clear();
    m_tangentsCount = 0;

    m_submeshes.clear();
    m_faceSplitIndices.clear();
    m_splits.clear();
    m_indicesSwapedFaceWinding.clear();
    m_UvIndicesSwapedFaceWinding.clear();
}

int Topology::getTriangulatedIndexCount() const
{
    return m_triangulatedIndexCount;
}

int Topology::getSplitCount() const
{
    return (int) m_splits.size();
}

int Topology::getSplitCount(aiPolyMeshSample * meshSample, bool forceRefresh)
{
    if (m_vertexCountPerFace && m_faceIndices) {
        if (m_faceSplitIndices.size() != m_vertexCountPerFace->size() || forceRefresh) {
            updateSplits(meshSample);
        }
    }
    else {
        m_splits.clear();
        m_faceSplitIndices.clear();
    }

    return (int) m_splits.size();
}

void Topology::updateSplits(aiPolyMeshSample * meshSample)
{
    DebugLog("Topology::updateSplits()");
    
    int splitIndex = 0;
    int indexOffset = 0;
    int faceCount = (int)m_vertexCountPerFace->size();

    m_faceSplitIndices.resize(faceCount); // number of faces

    m_splits.clear();

    if (m_vertexSharingEnabled && meshSample != nullptr && !meshSample->m_ownTopology) // only fixed topologies get this execution path
    {
        m_splits.push_back(SplitInfo());

        SplitInfo *curSplit = &(m_splits.back());
        for (size_t i = 0; i<faceCount; ++i)
            m_faceSplitIndices[i] = 0;

        curSplit->lastFace = faceCount-1;
        curSplit->vertexCount = (int)m_FixedTopoPositionsIndexes.size();
    }
    else
    {
        const size_t maxVertexSplitCount = m_use32BitsIndexBuffer ? MAX_VERTEX_SPLIT_COUNT_32 : MAX_VERTEX_SPLIT_COUNT_16;
        m_splits.reserve(1 + m_faceIndices->size() / maxVertexSplitCount);
        m_splits.push_back(SplitInfo());

        SplitInfo *curSplit = &(m_splits.back());
        for (int i = 0; i<faceCount; ++i) {
            int nv = m_vertexCountPerFace->get()[i];

            if (curSplit->vertexCount + nv > maxVertexSplitCount) {
                m_splits.push_back(SplitInfo(i, indexOffset));
                ++splitIndex;

                curSplit = &(m_splits.back());
            }

            m_faceSplitIndices[i] = splitIndex; // assign a split ID/index to each face

            curSplit->lastFace = i;
            curSplit->vertexCount += nv;

            indexOffset += nv;
        }
    }
}

int Topology::getVertexBufferLength(int splitIndex) const
{
    if (splitIndex < 0 || size_t(splitIndex) >= m_splits.size()) {
        return 0;
    }
    else {
        return (int) m_splits[splitIndex].vertexCount;
    }
}

int Topology::prepareSubmeshes(abcFaceSetSamples& face_sets, aiPolyMeshSample* sample)
{
    DebugLog("Topology::prepareSubmeshes()");
    
    const auto *counts = m_vertexCountPerFace->get();
    const auto *indices = m_FaceIndexingReindexed.data();
    int num_counts = (int)m_vertexCountPerFace->size();
    int num_indices = (int)m_FaceIndexingReindexed.size();
    int num_submeshes = (int)face_sets.size();

    m_submeshes.clear();
    int num_splits = getSplitCount(sample, false);
    if (face_sets.empty()) {
        for (int spi = 0; spi < num_splits; ++spi) {
            auto& split = m_splits[spi];
            split.submeshCount = 1;

            auto submesh = new Submesh();
            m_submeshes.emplace_back(submesh);
            submesh->splitIndex = spi;
            submesh->submeshIndex = 0;
            for (int fi = split.firstFace; fi <= split.lastFace; ++fi) {
                int count = counts[fi];
                if (count < 3)
                    continue;

                submesh->triangleCount += (count - 2);
            }
        }
    }
    else {
        RawVector<int> face_to_submesh;
        RawVector<int> face_to_offset;
        {
            // setup table
            face_to_offset.resize_discard(num_counts);
            int total = 0;
            for (int fi = 0; fi < num_counts; ++fi) {
                face_to_offset[fi] = total;
                total += counts[fi];
            }

            face_to_submesh.resize_discard(num_counts);
            for (int smi = 0; smi < num_submeshes; ++smi) {
                auto& smfaces = *face_sets[smi].getFaces();
                int num_counts = (int)smfaces.size();
                for (int ci = 0; ci < num_counts; ++ci) {
                    face_to_submesh[smfaces[ci]] = smi;
                }
            }
        }

        // create submeshes
        for (int spi = 0; spi < num_splits; ++spi) {
            m_splits[spi].submeshCount = num_submeshes;
            for (int smi = 0; smi < num_submeshes; ++smi) {
                auto submesh = new Submesh();
                m_submeshes.emplace_back(submesh);
                submesh->splitIndex = spi;
                submesh->submeshIndex = smi;
            }
        }

        // count how many triangles & indices each submesh has
        for (int spi = 0; spi < num_splits; ++spi) {
            int submesh_offset = num_submeshes * spi;
            auto& split = m_splits[spi];
            for (int fi = split.firstFace; fi <= split.lastFace; ++fi) {
                int count = counts[fi];
                if (count < 3)
                    continue;

                auto& submesh = *m_submeshes[face_to_submesh[fi] + submesh_offset];
                submesh.triangleCount += (count - 2);
                submesh.indexCount += count;
                submesh.faceCount++;
            }
        }

        // allocate indices
        for (auto& submesh : m_submeshes) {
            submesh->vertexIndices.resize_discard(submesh->indexCount);
            submesh->faces.resize_discard(submesh->faceCount);
            submesh->indexCount = 0;
            submesh->faceCount = 0;
        }

        // copy indices
        for (int spi = 0; spi < num_splits; ++spi) {
            int submesh_offset = num_submeshes * spi;
            auto& split = m_splits[spi];
            for (int fi = split.firstFace; fi <= split.lastFace; ++fi) {
                int count = counts[fi];
                if (count < 3)
                    continue;

                auto& submesh = *m_submeshes[face_to_submesh[fi] + submesh_offset];
                int offset = face_to_offset[fi];
                std::copy(indices + offset, indices + (offset + count), &submesh.vertexIndices[submesh.indexCount]);
                submesh.indexCount += count;
                submesh.faces[submesh.faceCount++] = count;
            }
        }
    }
    return (int) m_submeshes.size();
}

int Topology::getSplitSubmeshCount(int splitIndex) const
{
    if (splitIndex < 0 || size_t(splitIndex) >= m_splits.size()) {
        return 0;
    }
    else {
        return (int) m_splits[splitIndex].submeshCount;
    }
}

aiPolyMeshSample::aiPolyMeshSample(aiPolyMesh *schema, TopologyPtr topo, bool ownTopo)
    : super(schema)
    , m_topology(topo)
    , m_ownTopology(ownTopo)
{
}

aiPolyMeshSample::~aiPolyMeshSample()
{
}

bool aiPolyMeshSample::hasNormals() const
{
    switch (m_config.normalsMode)
    {
    case aiNormalsMode::ReadFromFile:
        return m_normals.valid();
    case aiNormalsMode::Ignore:
        return false;
    default:
        return (m_normals.valid() || !m_smoothNormals.empty());
    }
}

bool aiPolyMeshSample::hasUVs() const
{
    return m_uvs.valid();
}

bool aiPolyMeshSample::hasVelocities() const
{
    return !m_schema->hasVaryingTopology() && m_config.interpolateSamples;
}

bool aiPolyMeshSample::hasTangents() const
{
    return (m_config.tangentsMode != aiTangentsMode::None && hasUVs() && !m_tangents.empty() && !m_topology->m_tangentIndices.empty());
}

bool aiPolyMeshSample::smoothNormalsRequired() const
{
    return (m_config.normalsMode == aiNormalsMode::AlwaysCompute ||
            m_config.tangentsMode == aiTangentsMode::Smooth ||
            (!m_normals.valid() && m_config.normalsMode == aiNormalsMode::ComputeIfMissing));
}

bool aiPolyMeshSample::tangentsRequired() const
{
    return (m_config.tangentsMode != aiTangentsMode::None);
}

void aiPolyMeshSample::computeSmoothNormals(const aiConfig &config)
{
    DebugLog("%s: Compute smooth normals", getSchema()->getObject()->getFullName());

    size_t smoothNormalsCount = m_positions->size();
    m_smoothNormals.resize_zeroclear(smoothNormalsCount);

    const auto &counts = *(m_topology->m_vertexCountPerFace);
    const auto &indices = *(m_topology->m_faceIndices);
    const auto &positions = *m_positions;

    size_t nf = counts.size();
    size_t off = 0;
    bool ccw = config.swapFaceWinding;
    int ti1 = ccw ? 2 : 1;
    int ti2 = ccw ? 1 : 2;
    abcV3 N, dP1, dP2;

    for (size_t f=0; f<nf; ++f) {
        int nfv = counts[f];
        if (nfv >= 3) {
            // Compute average normal for current face
            N.setValue(0.0f, 0.0f, 0.0f);
            const abcV3 &P0 = positions[indices[off]];
            for (int fv=0; fv<nfv-2; ++fv) {
                auto &P1 = positions[indices[off + fv + ti1]];
                auto &P2 = positions[indices[off + fv + ti2]];

                dP1 = P1 - P0;
                dP2 = P2 - P0;
                
                N += dP2.cross(dP1).normalize();
            }

            if (nfv > 3) {
                N.normalize();
            }

            // Accumulate for all vertices participating to this face
            for (int fv=0; fv<nfv; ++fv) {
                m_smoothNormals[indices[off + fv]] += N;
            }
        }

        off += nfv;
    }

    // Normalize normal vectors
    for (abcV3& v : m_smoothNormals) { v.normalize(); }
}

void aiPolyMeshSample::computeTangentIndices(const aiConfig &config, const abcV3 *inN, bool indexedNormals) const
{
    const auto &counts = *(m_topology->m_vertexCountPerFace);
    const auto &indices = *(m_topology->m_faceIndices);
    const auto &uvVals = *(m_uvs.getVals());
    const auto &uvIdxs = *(m_uvs.getIndices());
    const auto *Nidxs = (indexedNormals ? m_normals.getIndices()->get() : 0);

    size_t tangentIndicesCount = indices.size();
    m_topology->m_tangentIndices.resize(tangentIndicesCount);
    
    if (config.tangentsMode == aiTangentsMode::Smooth) {
        for (size_t i=0; i<tangentIndicesCount; ++i) {
            m_topology->m_tangentIndices[i] = indices[i];
        }

        m_topology->m_tangentsCount = (int)m_positions->size();
    }
    else
    {
        TangentIndexMap uniqueIndices;
        TangentIndexMap::iterator it;

        size_t nf = counts.size();
        for (size_t f=0, v=0; f<nf; ++f) {
            int nfv = counts[f];
            for (int fv=0; fv<nfv; ++fv, ++v) {
                TangentKey key(inN[Nidxs ? Nidxs[v] : indices[v]], uvVals[uvIdxs[v]]);
                it = uniqueIndices.find(key);
                if (it == uniqueIndices.end()) {
                    int idx = (int) uniqueIndices.size();
                    m_topology->m_tangentIndices[v] = idx;
                    uniqueIndices[key] = idx;
                }
                else {
                    m_topology->m_tangentIndices[v] = it->second;
                }
            }
        }

        m_topology->m_tangentsCount = (int)uniqueIndices.size(); 
    }

    DebugLog("%lu unique tangent(s)", m_topology->m_tangentsCount);
}

void aiPolyMeshSample::computeTangents(const aiConfig &config, const abcV3 *inN, bool indexedNormals)
{
    DebugLog("%s: Compute %stangents", getSchema()->getObject()->getFullName(), (config.tangentsMode == aiTangentsMode::Smooth ? "smooth " : ""));

    const auto &counts = *(m_topology->m_vertexCountPerFace);
    const auto &indices = *(m_topology->m_faceIndices);
    const auto &positions = *m_positions;
    const auto &uvVals = *(m_uvs.getVals());
    const auto &uvIdxs = *(m_uvs.getIndices());
    const Util::uint32_t *Nidxs = (indexedNormals ? m_normals.getIndices()->get() : 0);

    size_t nf = counts.size();
    size_t off = 0;
    bool ccw = config.swapFaceWinding;
    int ti1 = (ccw ? 2 : 1);
    int ti2 = (ccw ? 1 : 2);

    size_t tangentsCount = m_topology->m_tangentsCount;
    m_tangents.resize_zeroclear(tangentsCount);

    RawVector<int> tanNidxs(tangentsCount);
    RawVector<abcV3> tan1(tangentsCount);
    RawVector<abcV3> tan2(tangentsCount);
    tanNidxs.zeroclear();
    tan1.zeroclear();
    tan2.zeroclear();

    abcV3 T, B, dP1, dP2, tmp;
    abcV2 dUV1, dUV2;

    for (size_t f=0; f<nf; ++f)
    {
        int nfv = counts[f];

        if (nfv >= 3)
        {
            // reset face tangent and bitangent
            T.setValue(0.0f, 0.0f, 0.0f);
            B.setValue(0.0f, 0.0f, 0.0f);

            const abcV3 &P0 = positions[indices[off]];
            const abcV2 &UV0 = uvVals[uvIdxs[off]];

            // for each triangle making up current polygon
            for (int fv=0; fv<nfv-2; ++fv)
            {
                const abcV3 &P1 = positions[indices[off + fv + ti1]];
                const abcV3 &P2 = positions[indices[off + fv + ti2]];

                const abcV2 &UV1 = uvVals[uvIdxs[off + fv + ti1]];
                const abcV2 &UV2 = uvVals[uvIdxs[off + fv + ti2]];

                dP1 = P1 - P0;
                dP2 = P2 - P0;
                
                dUV1 = UV1 - UV0;
                dUV2 = UV2 - UV0;

                float r = dUV1.x * dUV2.y - dUV1.y * dUV2.x;

                if (r != 0.0f)
                {
                    r = 1.0f / r;
                    
                    tmp.setValue(r * (dUV2.y * dP1.x - dUV1.y * dP2.x),
                                 r * (dUV2.y * dP1.y - dUV1.y * dP2.y),
                                 r * (dUV2.y * dP1.z - dUV1.y * dP2.z));
                    tmp.normalize();
                    // accumulate face tangent
                    T += tmp;

                    tmp.setValue(r * (dUV1.x * dP2.x - dUV2.x * dP1.x),
                                 r * (dUV1.x * dP2.y - dUV2.x * dP1.y),
                                 r * (dUV1.x * dP2.z - dUV2.x * dP1.z));
                    tmp.normalize();
                    // accumulte face bitangent
                    B += tmp;
                }
            }

            // normalize face tangent and bitangent if current polygon had to be splitted
            //   into several triangles
            if (nfv > 3)
            {
                T.normalize();
                B.normalize();
            }

            // accumulate normals, tangent and bitangent for each vertex
            for (int fv=0; fv<nfv; ++fv)
            {
                int v = m_topology->m_tangentIndices[off + fv];
                tan1[v] += T;
                tan2[v] += B;
                tanNidxs[v] = (Nidxs ? Nidxs[off + fv] : indices[off + fv]);
            }
        }

        off += nfv;
    }

    // compute final tangent space for each point
    for (size_t i=0; i<tangentsCount; ++i)
    {
        const abcV3 &Nv = inN[tanNidxs[i]];
        abcV3 &Tv = tan1[i];
        abcV3 &Bv = tan2[i];

        // Normalize Tv and Bv?
        
        T = Tv - Nv * Tv.dot(Nv);
        T.normalize();

        m_tangents[i].x = T.x;
        m_tangents[i].y = T.y;
        m_tangents[i].z = T.z;
        m_tangents[i].w = (Nv.cross(Tv).dot(Bv) < 0.0f
                            ? (m_config.swapHandedness ?  1.0f : -1.0f)
                            : (m_config.swapHandedness ? -1.0f :  1.0f));
    }
}

void aiPolyMeshSample::updateConfig(const aiConfig &config, bool &topoChanged, bool &dataChanged)
{
    DebugLog("aiPolyMeshSample::updateConfig()");
    
    topoChanged = (config.swapFaceWinding != m_config.swapFaceWinding);
    dataChanged = (config.swapHandedness != m_config.swapHandedness);

    bool smoothNormalsRequired = (config.normalsMode == aiNormalsMode::AlwaysCompute ||
                                  config.tangentsMode == aiTangentsMode::Smooth ||
                                  (!m_normals.valid() && config.normalsMode == aiNormalsMode::ComputeIfMissing));
    
    if (smoothNormalsRequired) {
        if (m_smoothNormals.empty() || topoChanged) {
            computeSmoothNormals(config);
            dataChanged = true;
        }
    }
    else {
        if (!m_smoothNormals.empty()) {
            DebugLog("%s: Clear smooth normals", getSchema()->getObject()->getFullName());
            m_smoothNormals.clear();
            dataChanged = true;
        }
    }

    bool tangentsRequired = (m_uvs.valid() && config.tangentsMode != aiTangentsMode::None);

    if (tangentsRequired) {
        bool tangentsModeChanged = (config.tangentsMode != m_config.tangentsMode);

        const abcV3 *N = nullptr;
        bool Nindexed = false;

        if (smoothNormalsRequired) {
            N = m_smoothNormals.data();
        }
        else if (m_normals.valid()) {
            N = m_normals.getVals()->get();
            Nindexed = (m_normals.getScope() == AbcGeom::kFacevaryingScope);
        }

        if (N) {
            // do not compute indices if they are cached, constant topology and valid
            if (m_topology->m_tangentIndices.empty() ||
                !config.cacheTangentsSplits ||
                tangentsModeChanged)
            {
                computeTangentIndices(config, N, Nindexed);
            }
            if (m_tangents.empty() || 
                tangentsModeChanged ||
                topoChanged)
            {
                computeTangents(config, N, Nindexed);
                dataChanged = true;
            }
        }
        else {
            tangentsRequired = false;
        }
    }
    
    if (!tangentsRequired) {
        if (!m_tangents.empty()) {
            DebugLog("%s: Clear tangents", getSchema()->getObject()->getFullName());

            m_tangents.clear();
            dataChanged = true;
        }

        if (!m_topology->m_tangentIndices.empty() && (m_ownTopology || !config.cacheTangentsSplits))
        {
            DebugLog("%s: Clear tangent indices", getSchema()->getObject()->getFullName());

            m_topology->m_tangentIndices.clear();
            m_topology->m_tangentsCount = 0;
        }
    }

    if (topoChanged)
    {
        dataChanged = true;
    }

    m_config = config;   
}

void aiPolyMeshSample::getSummary(bool forceRefresh, aiMeshSampleSummary &summary, aiPolyMeshSample* sample) const
{
    DebugLog("aiPolyMeshSample::getSummary(forceRefresh=%s)", forceRefresh ? "true" : "false");
    
    summary.splitCount = m_topology->getSplitCount(sample, forceRefresh);
    summary.hasNormals = hasNormals();
    summary.hasUVs = hasUVs();
    summary.hasTangents = hasTangents();
    summary.hasVelocities = hasVelocities();
}


void aiPolyMeshSample::getDataPointer(aiPolyMeshData &dst) const
{
    if (m_positions) {
        dst.positionCount = m_positions->valid() ? (int)m_positions->size() : 0;
        dst.positions = (abcV3*)(m_positions->get());
    }

    if (m_velocities) {
        dst.velocities = m_velocities->valid() ? (abcV3*)m_velocities->get() : nullptr;
    }

    if (m_normals) {
        dst.normalCount = (int)m_normals.getVals()->size();
        dst.normals = (abcV3*)m_normals.getVals()->get();
        dst.normalIndexCount = m_normals.isIndexed() ? (int)m_normals.getIndices()->size() : 0;
        if (dst.normalIndexCount) {
            dst.normalIndices = (int*)m_normals.getIndices()->get();
        }
    }

    if (m_uvs) {
        dst.uvCount = (int)m_uvs.getVals()->size();
        dst.uvs = (abcV2*)m_uvs.getVals()->get();
        dst.uvIndexCount = m_uvs.isIndexed() ? (int)m_uvs.getIndices()->size() : 0;
        if (dst.uvIndexCount) {
            dst.uvIndices = (int*)m_uvs.getIndices()->get();
        }
    }

    if (m_topology) {
        if (m_topology->m_faceIndices) {
            dst.indexCount = (int)m_topology->m_faceIndices->size();
            dst.indices = (int*)m_topology->m_faceIndices->get();
        }
        if (m_topology->m_vertexCountPerFace) {
            dst.faceCount = (int)m_topology->m_vertexCountPerFace->size();
            dst.faces = (int*)m_topology->m_vertexCountPerFace->get();
            dst.triangulatedIndexCount = m_topology->m_triangulatedIndexCount;
        }
    }

    dst.center = m_bounds.center();
    dst.size = m_bounds.size();
}

void aiPolyMeshSample::copyData(aiPolyMeshData &dst)
{
    aiPolyMeshData src;
    getDataPointer(src);

    // sadly, memcpy() is way faster than std::copy() on VC

    if (src.faces && dst.faces && dst.faceCount >= src.faceCount) {
        memcpy(dst.faces, src.faces, src.faceCount * sizeof(*dst.faces));
        dst.faceCount = src.faceCount;
    }
    else {
        dst.faceCount = 0;
    }

    if (src.positions && dst.positions && dst.positionCount >= src.positionCount) {
        memcpy(dst.positions, src.positions, src.positionCount * sizeof(*dst.positions));
        dst.positionCount = src.positionCount;
    }
    else {
        dst.positionCount = 0;
    }

    if (src.velocities && dst.velocities && dst.positionCount >= src.positionCount) {
        memcpy(dst.velocities, src.velocities, src.positionCount * sizeof(*dst.velocities));
    }

    if (src.interpolatedVelocitiesXY && dst.interpolatedVelocitiesXY && dst.positionCount >= src.positionCount)
    {
        memcpy(dst.interpolatedVelocitiesXY, src.interpolatedVelocitiesXY, src.positionCount * sizeof(*dst.interpolatedVelocitiesXY));
    }

    if (src.interpolatedVelocitiesZ && dst.interpolatedVelocitiesZ && dst.positionCount >= src.positionCount)
    {
        memcpy(dst.interpolatedVelocitiesZ, src.interpolatedVelocitiesZ, src.positionCount * sizeof(*dst.interpolatedVelocitiesZ));
    }
    

    if (src.normals && dst.normals && dst.normalCount >= src.normalCount) {
        memcpy(dst.normals, src.normals, src.normalCount * sizeof(*dst.normals));
        dst.normalCount = src.normalCount;
    }
    else {
        dst.normalCount = 0;
    }

    if (src.uvs && dst.uvs && dst.uvCount >= src.uvCount) {
        memcpy(dst.uvs, src.uvs, src.uvCount * sizeof(*dst.uvs));
        dst.uvCount = src.uvCount;
    }
    else {
        dst.uvCount = 0;
    }

    auto copy_indices = [&](int *d, const int *s, int n) {
        memcpy(d, s, n * sizeof(int));
    };

    if (src.indices && dst.indices && dst.indexCount >= src.indexCount) {
        copy_indices(dst.indices, src.indices, src.indexCount);
        dst.indexCount = src.indexCount;
    }
    if (src.normalIndices && dst.normalIndices && dst.normalIndexCount >= src.normalIndexCount) {
        copy_indices(dst.normalIndices, src.normalIndices, src.normalIndexCount);
        dst.normalIndexCount = src.normalIndexCount;
    }
    if (src.uvIndices && dst.uvIndices && dst.uvIndexCount >= src.uvIndexCount) {
        copy_indices(dst.uvIndices, src.uvIndices, src.uvIndexCount);
        dst.uvIndexCount = src.uvIndexCount;
    }

    dst.center = dst.center;
    dst.size = dst.size;
}

int aiPolyMeshSample::getVertexBufferLength(int splitIndex) const
{
    DebugLog("aiPolyMeshSample::getVertexBufferLength(splitIndex=%d)", splitIndex);
    
    return m_topology->getVertexBufferLength(splitIndex);
}

void aiPolyMeshSample::fillVertexBuffer(int splitIndex, aiPolyMeshData &data)
{
    DebugLog("aiPolyMeshSample::fillVertexBuffer(splitIndex=%d)", splitIndex);
    
    if (splitIndex < 0 || size_t(splitIndex) >= m_topology->m_splits.size() || m_topology->m_splits[splitIndex].vertexCount == 0)
    {
        return;
    }

    bool copyNormals = (hasNormals() && data.normals);
    bool copyUvs = (hasUVs() && data.uvs);
    bool copyTangents = (hasTangents() && data.tangents);
    
    bool useAbcNormals = (m_normals.valid() && (m_config.normalsMode == aiNormalsMode::ReadFromFile || m_config.normalsMode == aiNormalsMode::ComputeIfMissing));
    float xScale = (m_config.swapHandedness ? -1.0f : 1.0f);
    bool interpolatePositions = hasVelocities() && m_nextPositions != nullptr;
    float timeOffset = static_cast<float>(m_currentTimeOffset);
    float timeInterval = static_cast<float>(m_currentTimeInterval);
    float vertexMotionScale = static_cast<float>(m_config.vertexMotionScale);
    
    const SplitInfo &split = m_topology->m_splits[splitIndex];
    const auto *faceCount = m_topology->m_vertexCountPerFace->get();
    const auto *indices = m_config.turnQuadEdges ? m_topology->m_indicesSwapedFaceWinding.data() : m_topology->m_faceIndices->get();
    const auto *positions = m_positions->get();
    const auto *nextPositions = m_nextPositions->get();

    size_t k = 0;
    size_t o = split.indexOffset;
    
    // reset unused data arrays

    if (data.normals && !copyNormals)
    {
        DebugLog("%s: Reset normals", getSchema()->getObject()->getFullName());
        memset(data.normals, 0, split.vertexCount * sizeof(abcV3));
    }
    
    if (data.uvs && !copyUvs)
    {
        DebugLog("%s: Reset UVs", getSchema()->getObject()->getFullName());
        memset(data.uvs, 0, split.vertexCount * sizeof(abcV2));
    }
    
    if (data.tangents && !copyTangents)
    {
        DebugLog("%s: Reset tangents", getSchema()->getObject()->getFullName());
        memset(data.tangents, 0, split.vertexCount * sizeof(abcV4));
    }

    abcV3 bbmin = positions[indices[o]];
    abcV3 bbmax = bbmin;

#define UPDATE_POSITIONS_AND_BOUNDS(srcIdx, dstIdx) \
    abcV3 &cP = data.positions[dstIdx]; \
    cP = positions[srcIdx]; \
    if (interpolatePositions) \
    {\
        abcV3 distance = nextPositions[srcIdx] - positions[srcIdx]; \
        abcV3 velocity = (distance / timeInterval) * vertexMotionScale; \
        cP+= distance * timeOffset; \
        data.interpolatedVelocitiesXY[dstIdx].x = velocity.x*xScale; \
        data.interpolatedVelocitiesXY[dstIdx].y = velocity.y; \
        data.interpolatedVelocitiesZ[dstIdx].x = velocity.z; \
    }\
    cP.x *= xScale; \
    if (cP.x < bbmin.x) bbmin.x = cP.x; \
    else if (cP.x > bbmax.x) bbmax.x = cP.x; \
    if (cP.y < bbmin.y) bbmin.y = cP.y; \
    else if (cP.y > bbmax.y) bbmax.y = cP.y; \
    if (cP.z < bbmin.z) bbmin.z = cP.z; \
    else if (cP.z > bbmax.z) bbmax.z = cP.z


    // fill data arrays
    if ( m_topology->m_vertexSharingEnabled && m_topology->m_TreatVertexExtraDataAsStatic && !m_topology->m_FreshlyReadTopologyData && m_topology->m_FixedTopoPositionsIndexes.size())
    {
        for (size_t i = 0; i < m_topology->m_FixedTopoPositionsIndexes.size(); i++)
        {
            UPDATE_POSITIONS_AND_BOUNDS(m_topology->m_FixedTopoPositionsIndexes[i], i);
        }
    }
    else
    {
        m_topology->m_FreshlyReadTopologyData = false;
        if (copyNormals) {
            if (useAbcNormals) {
                const auto *normals = m_normals.getVals()->get();

                if (m_normals.getScope() == AbcGeom::kFacevaryingScope) {
                    const auto &nIndices = *(m_normals.getIndices());

                    if (copyUvs) {
                        const auto *uvs = m_uvs.getVals()->get();
                        const auto *uvIndices = m_config.turnQuadEdges ? m_topology->m_UvIndicesSwapedFaceWinding.data() : m_uvs.getIndices()->get();

                        if (copyTangents) {
                            for (size_t i = split.firstFace; i <= split.lastFace; ++i) {
                                int nv = faceCount[i];
                                for (int j = 0; j < nv; ++j, ++o, ++k) {
                                    if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                        size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                        size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                        UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                        data.normals[dstNdx] = normals[nIndices[o]];
                                        data.normals[dstNdx].x *= xScale;
                                        data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                        data.tangents[dstNdx].x *= xScale;
                                        data.uvs[dstNdx] = uvs[uvIndices[o]];
                                    }
                                    else {
                                        UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                                        data.normals[k] = normals[nIndices[o]];
                                        data.normals[k].x *= xScale;
                                        data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                        data.tangents[k].x *= xScale;
                                        data.uvs[k] = uvs[uvIndices[o]];
                                    }
                                }
                            }
                        }
                        else {
                            for (size_t i = split.firstFace; i <= split.lastFace; ++i) {
                                int nv = faceCount[i];
                                for (int j = 0; j < nv; ++j, ++o, ++k) {
                                    if( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                        size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                        size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                        UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                        data.normals[dstNdx] = normals[nIndices[o]];
                                        data.normals[dstNdx].x *= xScale;
                                        data.uvs[dstNdx] = uvs[uvIndices[o]];
                                    }
                                    else {
                                        UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                                        data.normals[k] = normals[nIndices[o]];
                                        data.normals[k].x *= xScale;
                                        data.uvs[k] = uvs[uvIndices[o]];
                                    }
                                }
                            }
                        }
                    }
                    else if (copyTangents) {
                        for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                            int nv = faceCount[i];
                            for (int j = 0; j < nv; ++j, ++o, ++k) {
                                if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                    size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                    size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                    UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                    data.normals[dstNdx] = normals[nIndices[o]];
                                    data.normals[dstNdx].x *= xScale;
                                    data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                    data.tangents[dstNdx].x *= xScale;
                                }
                                else {
                                    UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                                    data.normals[k] = normals[nIndices[o]];
                                    data.normals[k].x *= xScale;
                                    data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                    data.tangents[k].x *= xScale;
                                }
                            }
                        }
                    }
                    else {
                        for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                            int nv = faceCount[i];
                            for (int j = 0; j < nv; ++j, ++o, ++k) {
                                if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                    size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                    size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                    UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                    data.normals[dstNdx] = normals[nIndices[o]];
                                    data.normals[dstNdx].x *= xScale;
                                }
                                else {
                                    UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                                    data.normals[k] = normals[nIndices[o]];
                                    data.normals[k].x *= xScale;
                                }
                            }
                        }
                    }
                }
                else {
                    if (copyUvs) {
                        const auto *uvs = m_uvs.getVals()->get();
                        const auto *uvIndices = m_config.turnQuadEdges ? m_topology->m_UvIndicesSwapedFaceWinding.data() : m_uvs.getIndices()->get();
                    
                        if (copyTangents) {
                            for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                                int nv = faceCount[i];
                                for (int j = 0; j < nv; ++j, ++o, ++k) {
                                    if (m_topology->m_FixedTopoPositionsIndexes.size()) {
                                        size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                        size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                        UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                        data.normals[dstNdx] = normals[indices[o]];
                                        data.normals[dstNdx].x *= xScale;
                                        data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                        data.tangents[dstNdx].x *= xScale;
                                        data.uvs[dstNdx] = uvs[uvIndices[o]];
                                    }
                                    else {
                                        int v = indices[o];
                                        UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                        data.normals[k] = normals[v];
                                        data.normals[k].x *= xScale;
                                        data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                        data.tangents[k].x *= xScale;
                                        data.uvs[k] = uvs[uvIndices[o]];
                                    }
                                }
                            }
                        }
                        else {
                            for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                                int nv = faceCount[i];
                                for (int j = 0; j < nv; ++j, ++o, ++k) {
                                    if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                        size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                        size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                        UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                        data.normals[dstNdx] = normals[indices[o]];
                                        data.normals[dstNdx].x *= xScale;
                                        data.uvs[dstNdx] = uvs[uvIndices[o]];
                                    }
                                    else {
                                        int v = indices[o];
                                        UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                        data.normals[k] = normals[v];
                                        data.normals[k].x *= xScale;
                                        data.uvs[k] = uvs[uvIndices[o]];
                                    }
                                }
                            }
                        }
                    }
                    else if (copyTangents) {
                        for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                            int nv = faceCount[i];
                            for (int j = 0; j < nv; ++j, ++o, ++k) {
                                if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                    size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                    size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                    UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                    data.normals[dstNdx] = normals[indices[o]];
                                    data.normals[dstNdx].x *= xScale;
                                    data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                    data.tangents[dstNdx].x *= xScale;
                                }
                                else {
                                    int v = indices[o];
                                    UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                    data.normals[k] = normals[v];
                                    data.normals[k].x *= xScale;
                                    data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                    data.tangents[k].x *= xScale;
                                }
                            }
                        }
                    }
                    else {
                        for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                            int nv = faceCount[i];
                            for (int j = 0; j < nv; ++j, ++o, ++k) {
                                if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                    size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                    size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                    UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                    data.normals[dstNdx] = normals[indices[o]];
                                    data.normals[dstNdx].x *= xScale;
                                }
                                else {
                                    int v = indices[o];
                                    UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                    data.normals[k] = normals[v];
                                    data.normals[k].x *= xScale;
                                }
                            }
                        }
                    }
                }
            }
            else {
                if (copyUvs) {
                    const auto *uvs = m_uvs.getVals()->get();
                    const auto *uvIndices = m_config.turnQuadEdges ? m_topology->m_UvIndicesSwapedFaceWinding.data() : m_uvs.getIndices()->get();
                
                    if (copyTangents) {
                        for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                            int nv = faceCount[i];
                            for (int j = 0; j < nv; ++j, ++o, ++k) {
                                if ( m_topology->m_FixedTopoPositionsIndexes.size())
                                {
                                    size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                    size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                    UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                    data.normals[dstNdx] = m_smoothNormals[indices[o]];
                                    data.normals[dstNdx].x *= xScale;
                                    data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                    data.tangents[dstNdx].x *= xScale;
                                    data.uvs[dstNdx] = uvs[uvIndices[o]];
                                }
                                else {
                                    int v = indices[o];

                                    UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                    data.normals[k] = m_smoothNormals[v];
                                    data.normals[k].x *= xScale;
                                    data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                    data.tangents[k].x *= xScale;
                                    data.uvs[k] = uvs[uvIndices[o]];
                                }
                            }
                        }
                    }
                    else {
                        for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                            int nv = faceCount[i];
                            for (int j = 0; j < nv; ++j, ++o, ++k) {
                                if (m_topology->m_FixedTopoPositionsIndexes.size()) {
                                    size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                    size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                    UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                    data.normals[dstNdx] = m_smoothNormals[indices[o]];
                                    data.normals[dstNdx].x *= xScale;
                                    data.uvs[dstNdx] = uvs[uvIndices[o]];
                                }
                                else {
                                    int v = indices[o];
                                    UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                    data.normals[k] = m_smoothNormals[v];
                                    data.normals[k].x *= xScale;
                                    data.uvs[k] = uvs[uvIndices[o]];
                                }
                            }
                        }
                    }
                }
                else if (copyTangents) {
                    for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                        int nv = faceCount[i];
                        for (int j = 0; j < nv; ++j, ++o, ++k) {
                            if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                data.normals[dstNdx] = m_smoothNormals[indices[o]];
                                data.normals[dstNdx].x *= xScale;
                                data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                data.tangents[dstNdx].x *= xScale;
                            }
                            else {
                                int v = indices[o];
                                UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                data.normals[k] = m_smoothNormals[v];
                                data.normals[k].x *= xScale;
                                data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                data.tangents[k].x *= xScale;
                            }
                        }
                    }
                }
                else {
                    for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                        int nv = faceCount[i];
                        for (int j = 0; j < nv; ++j, ++o, ++k) {
                            if ( m_topology->m_FixedTopoPositionsIndexes.size()) {
                                size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                data.normals[dstNdx] = m_smoothNormals[indices[o]];
                                data.normals[dstNdx].x *= xScale;
                            }
                            else {
                                int v = indices[o];
                                UPDATE_POSITIONS_AND_BOUNDS(v, k);
                                data.normals[k] = m_smoothNormals[v];
                                data.normals[k].x *= xScale;
                            }
                        }
                    }
                }
            }
        }
        else {
            if (copyUvs) {
                const auto *uvs = m_uvs.getVals()->get();
                const auto *uvIndices = m_config.turnQuadEdges ? m_topology->m_UvIndicesSwapedFaceWinding.data() : m_uvs.getIndices()->get();
            
                if (copyTangents) {
                    for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                        int nv = faceCount[i];
                        for (int j = 0; j < nv; ++j, ++o, ++k) {
                            if (m_topology->m_FixedTopoPositionsIndexes.size()) {
                                size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                                data.tangents[dstNdx].x *= xScale;
                                data.uvs[dstNdx] = uvs[uvIndices[o]];
                            }
                            else {
                                UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                                data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                                data.tangents[k].x *= xScale;
                                data.uvs[k] = uvs[uvIndices[o]];
                            }
                        }
                    }
                }
                else {
                    for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                        int nv = faceCount[i];
                        for (int j = 0; j < nv; ++j, ++o, ++k) {
                            if (m_topology->m_FixedTopoPositionsIndexes.size()) {
                                size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                                size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                                UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                                data.uvs[dstNdx] = uvs[uvIndices[o]];
                            }
                            else {
                                UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                                data.uvs[k] = uvs[uvIndices[o]];
                            }
                        }
                    }
                }
            }
            else if (copyTangents) {
                for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                    int nv = faceCount[i];
                    for (int j = 0; j < nv; ++j, ++o, ++k) {
                        if (m_topology->m_FixedTopoPositionsIndexes.size()) {
                            size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                            size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                            UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                            data.tangents[dstNdx] = m_tangents[m_topology->m_tangentIndices[o]];
                            data.tangents[dstNdx].x *= xScale;
                        }
                        else {
                            UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                            data.tangents[k] = m_tangents[m_topology->m_tangentIndices[o]];
                            data.tangents[k].x *= xScale;
                        }
                    }
                }
            }
            else {
                for (size_t i=split.firstFace; i<=split.lastFace; ++i) {
                    int nv = faceCount[i];
                    for (int j = 0; j < nv; ++j, ++o, ++k) {
                        if (m_topology->m_FixedTopoPositionsIndexes.size()) {
                            size_t dstNdx = m_topology->m_FaceIndexingReindexed[o];
                            size_t srcNdx = m_topology->m_FixedTopoPositionsIndexes[dstNdx];

                            UPDATE_POSITIONS_AND_BOUNDS(srcNdx, dstNdx);
                        }
                        else
                        {
                            UPDATE_POSITIONS_AND_BOUNDS(indices[o], k);
                        }
                    }
                }
            }
        }
    }

#undef UPDATE_POSITIONS_AND_BOUNDS

    data.center = 0.5f * (bbmin + bbmax);
    data.size = bbmax - bbmin;
}

int aiPolyMeshSample::prepareSubmeshes(aiPolyMeshSample* sample)
{
    DebugLog("aiPolyMeshSample::prepateSubmeshes()");

    int rv = m_topology->prepareSubmeshes(m_facesets, sample);
    m_curSubmesh = m_topology->submeshBegin();
    return rv;
}

int aiPolyMeshSample::getSplitSubmeshCount(int splitIndex) const
{
    DebugLog("aiPolyMeshSample::getSplitSubmeshCount()");
    
    return m_topology->getSplitSubmeshCount(splitIndex);
}

bool aiPolyMeshSample::getNextSubmesh(aiSubmeshSummary &summary)
{
    DebugLog("aiPolyMeshSample::getNextSubmesh()");
    
    if (m_curSubmesh == m_topology->submeshEnd()) {
        return false;
    }
    else {
        Submesh &submesh = **m_curSubmesh;
        summary.index = int(m_curSubmesh - m_topology->submeshBegin());
        summary.splitIndex = submesh.splitIndex;
        summary.splitSubmeshIndex = submesh.submeshIndex;
        summary.triangleCount = int(submesh.triangleCount);

        ++m_curSubmesh;
        return true;
    }
}

void aiPolyMeshSample::fillSubmeshIndices(const aiSubmeshSummary &summary, aiSubmeshData &data) const
{
    DebugLog("aiPolyMeshSample::fillSubmeshIndices()");
    
    auto it = m_topology->submeshBegin() + summary.index;
    if (it != m_topology->submeshEnd()) {
        bool ccw = m_config.swapFaceWinding;
        const auto &counts = *(m_topology->m_vertexCountPerFace);
        const auto &submesh = **it;

        int index = 0;
        int i1 = (ccw ? 2 : 1);
        int i2 = (ccw ? 1 : 2);
        int offset = 0;
        
        if (submesh.faces.empty() && submesh.vertexIndices.empty()) {
            // single submesh case, faces and vertexIndices not populated
            for (size_t i=0; i<counts.size(); ++i) {
                int nv = counts[i];
                int nt = nv - 2;
                if (m_topology->m_FixedTopoPositionsIndexes.size() > 0) {
                    for (int ti = 0; ti<nt; ++ti) {
                        data.indices[offset + 0] = m_topology->m_FaceIndexingReindexed[index];
                        data.indices[offset + 1] = m_topology->m_FaceIndexingReindexed[index + ti + i1];
                        data.indices[offset + 2] = m_topology->m_FaceIndexingReindexed[index + ti + i2];
                        offset += 3;
                    }
                }
                else {
                    for (int ti = 0; ti<nt; ++ti) {
                        data.indices[offset + 0] = index;
                        data.indices[offset + 1] = index + ti + i1;
                        data.indices[offset + 2] = index + ti + i2;
                        offset += 3;
                    }
                }
                index += nv;
            }
        }
        else {
            for (int32_t f : submesh.faces) {
                int nv = counts[f];
                int nt = nv - 2;
                for (int ti = 0; ti < nt; ++ti) {
                    data.indices[offset + 0] = submesh.vertexIndices[index];
                    data.indices[offset + 1] = submesh.vertexIndices[index + ti + i1];
                    data.indices[offset + 2] = submesh.vertexIndices[index + ti + i2];
                    offset += 3;
                }
                index += nv;
            }
        }
    }
}

// ---

aiPolyMesh::aiPolyMesh(aiObject *obj)
    : super(obj)
    , m_peakIndexCount(0)
    , m_peakTriangulatedIndexCount(0)
    , m_peakVertexCount(0)
    , m_ignoreNormals(false)
    , m_ignoreUVs(false)
{
    m_constant = m_schema.isConstant();

    auto normals = m_schema.getNormalsParam();
    if (normals.valid()) {
        auto scope = normals.getScope();
        if (scope != AbcGeom::kUnknownScope) {
            if (!normals.isConstant()) {
                m_constant = false;
            }
        }
        else {
            m_ignoreNormals = true;
        }
    }

    auto uvs = m_schema.getUVsParam();
    if (uvs.valid()) {
        auto scope = uvs.getScope();
        if (scope != AbcGeom::kUnknownScope) {
            if (!uvs.isConstant()) {
                m_constant = false;
            }
        }
        else {
            m_ignoreUVs = true;
        }
    }

    m_varyingTopology = (m_schema.getTopologyVariance() == AbcGeom::kHeterogeneousTopology);

    // find FaceSetSchema in children
    size_t num_children = m_obj->getAbcObject().getNumChildren();
    for (size_t i = 0; i < num_children; ++i) {
        auto& child = m_obj->getAbcObject().getChild(i);
        if (!child.valid())
            continue;

        if (AbcGeom::IFaceSetSchema::matches(child.getMetaData())) {
            auto so = Abc::ISchemaObject<AbcGeom::IFaceSetSchema>(child, Abc::kWrapExisting);
            auto faceset = so.getSchema();
            // check if time sampling and variance are same
            if (faceset.isConstant() == m_schema.isConstant() &&
                faceset.getTimeSampling() == m_schema.getTimeSampling() &&
                faceset.getNumSamples() == m_schema.getNumSamples())
            {
                m_facesets.push_back(faceset);
            }
        }
    }

    DebugLog("aiPolyMesh::aiPolyMesh(constant=%s, varyingTopology=%s)",
             (m_constant ? "true" : "false"),
             (m_varyingTopology ? "true" : "false"));
}

aiPolyMesh::Sample* aiPolyMesh::newSample()
{
    Sample *sample = getSample();    
    if (!sample) {
        if (dontUseCache() || !m_varyingTopology) {
            if (!m_sharedTopology)
                m_sharedTopology.reset(new Topology());
            sample = new Sample(this, m_sharedTopology, false);
        }
        else {
            sample = new Sample(this, TopologyPtr(new Topology()), true);
        }
    }
    else {
        if (m_varyingTopology) {
            sample->m_topology->clear();
        }
    }
    return sample;
}

aiPolyMesh::Sample* aiPolyMesh::readSample(const uint64_t idx, bool &topologyChanged)
{
    auto ss = aiIndexToSampleSelector(idx);
    auto ss2 = aiIndexToSampleSelector(idx + 1);
    DebugLog("aiPolyMesh::readSample(t=%d)", idx);
    
    Sample *sample = newSample();
    auto topology = sample->m_topology;

    topologyChanged = m_varyingTopology;
    topology->EnableVertexSharing(m_config.shareVertices && !m_varyingTopology);
    topology->Enable32BitsIndexbuffers(m_config.use32BitsIndexBuffer);
    topology->TreatVertexExtraDataAsStatic(m_config.treatVertexExtraDataAsStatic && !m_varyingTopology);

    if (!topology->m_vertexCountPerFace || m_varyingTopology) {
        m_schema.getFaceCountsProperty().get(topology->m_vertexCountPerFace, ss);
        topology->m_triangulatedIndexCount = CalculateTriangulatedIndexCount(*topology->m_vertexCountPerFace);
        topologyChanged = true;
    }
    if (!topology->m_faceIndices || m_varyingTopology) {
        m_schema.getFaceIndicesProperty().get(topology->m_faceIndices, ss);
        topologyChanged = true;
    }
    if (topologyChanged && !m_facesets.empty()) {
        sample->m_facesets.resize(m_facesets.size());
        for (size_t fi = 0; fi < m_facesets.size(); ++fi) {
            m_facesets[fi].get(sample->m_facesets[fi], ss);
        }
    }
    m_schema.getPositionsProperty().get(sample->m_positions, ss);

    if (!m_varyingTopology && m_config.interpolateSamples) {
        m_schema.getPositionsProperty().get(sample->m_nextPositions, ss2);
    }

    sample->m_velocities.reset();
    auto velocitiesProp = m_schema.getVelocitiesProperty();
    if (velocitiesProp.valid()) {
        velocitiesProp.get(sample->m_velocities, ss);
    }

    bool smoothNormalsRequired = sample->smoothNormalsRequired();

    sample->m_normals.reset();
    auto normalsParam = m_schema.getNormalsParam();
    if (!m_ignoreNormals && normalsParam.valid()) {
        if (normalsParam.isConstant()) {
            if (!m_sharedNormals.valid()) {
                DebugLog("  Read normals (constant)");
                normalsParam.getIndexed(m_sharedNormals, ss);
            }
            sample->m_normals = m_sharedNormals;
        }
        else {
            DebugLog("  Read normals");
            normalsParam.getIndexed(sample->m_normals, ss);
        }
    }

    sample->m_uvs.reset();
    auto uvsParam = m_schema.getUVsParam();
    if (!m_ignoreUVs && uvsParam.valid()) {
        if (uvsParam.isConstant()) {
            if (!m_sharedUVs.valid()) {
                DebugLog("  Read uvs (constant)");
                uvsParam.getIndexed(m_sharedUVs, ss);
            }

            sample->m_uvs = m_sharedUVs;
        }
        else {
            DebugLog("  Read uvs");
            uvsParam.getIndexed(sample->m_uvs, ss);
        }
    }

    auto boundsParam = m_schema.getSelfBoundsProperty();
    if (boundsParam) {
        boundsParam.get(sample->m_bounds, ss);
    }

    if (smoothNormalsRequired) {
        sample->computeSmoothNormals(m_config);
    }

    if (sample->tangentsRequired()) {
        const abcV3 *normals = nullptr;
        bool indexedNormals = false;
        
        if (smoothNormalsRequired) {
            normals = sample->m_smoothNormals.data();
        }
        else if (sample->m_normals.valid()) {
            normals = sample->m_normals.getVals()->get();
            indexedNormals = (sample->m_normals.getScope() == AbcGeom::kFacevaryingScope);
        }

        if (normals && sample->m_uvs.valid()) {
            // topology may be shared, check tangent indices
            if (topology->m_tangentIndices.empty() || !m_config.cacheTangentsSplits) {
                sample->computeTangentIndices(m_config, normals, indexedNormals);
            }
            sample->computeTangents(m_config, normals, indexedNormals);
        }
    }

    if (m_config.turnQuadEdges) {
        if (m_varyingTopology || topology->m_indicesSwapedFaceWinding.size() == 0) {
            auto faces = topology->m_vertexCountPerFace;
            auto totalFaces = faces->size();
            
            auto * facesIndices = topology->m_faceIndices->get();
            topology->m_indicesSwapedFaceWinding.reserve(topology->m_faceIndices->size());
            
            auto index = 0;
            const uint32_t indexRemap[4] = {3,0,1,2};
            for (uint32_t faceIndex = 0; faceIndex < totalFaces; faceIndex++) {
                auto faceSize = faces->get()[faceIndex];
                if (faceSize == 4) {
                    for (auto i = 0; i < faceSize; i++) {
                        topology->m_indicesSwapedFaceWinding.push_back(facesIndices[index + indexRemap[i]]);
                    }
                }
                else {
                    for (auto i = 0; i < faceSize; i++) {
                        topology->m_indicesSwapedFaceWinding.push_back(facesIndices[index + i]);
                    }
                }
                index += faceSize;
            }

            if (sample->m_uvs.valid()) {
                index = 0;
                const auto& uvIndices = *sample->m_uvs.getIndices();
                topology->m_UvIndicesSwapedFaceWinding.reserve(sample->m_uvs.getIndices()->size());

                for (size_t faceIndex = 0; faceIndex < totalFaces; faceIndex++) {
                    auto faceSize = faces->get()[faceIndex];
                    if (faceSize == 4) {
                        for (auto i = 0; i < faceSize; i++) {
                            topology->m_UvIndicesSwapedFaceWinding.push_back(uvIndices[index + indexRemap[i]]);
                        }
                    }
                    else {
                        for (auto i = 0; i < faceSize; i++) {
                            topology->m_UvIndicesSwapedFaceWinding.push_back(uvIndices[index+i]);
                        }
                    }
                    index += faceSize;
                }
            }
        }
    }
    else if (topology->m_indicesSwapedFaceWinding.size()>0) {
        topology->m_indicesSwapedFaceWinding.clear();
    }

    if (m_config.shareVertices && !m_varyingTopology && sample != nullptr && !sample->m_ownTopology && topologyChanged)
        GenerateVerticesToFacesLookup(sample);

    return sample;
}

// generates two lookup tables:
//  m_FaceIndexingReindexed         : for each face in the abc sample, hold an index value to lookup in m_FixedTopoPositionsIndexes, that will give final position index.
//  m_FixedTopoPositionsIndexes     : list of resulting positions. value is index into the abc "position" vector. size is greter than or equal to "position" array.
void aiPolyMesh::GenerateVerticesToFacesLookup(aiPolyMeshSample *sample) const
{
    auto topology = sample->m_topology;
    auto  faces = topology->m_vertexCountPerFace;

    auto * facesIndices = m_config.turnQuadEdges ?
        topology->m_indicesSwapedFaceWinding.data() : topology->m_faceIndices->get();
    uint32_t totalFaces = static_cast<uint32_t>(faces->size());

    // 1st, figure out which face uses which vertices (for sharing identification)
    std::unordered_map< uint32_t, std::vector<uint32_t>> indexesOfFacesValues;
    uint32_t facesIndicesCursor = 0;
    for (uint32_t faceIndex = 0; faceIndex < totalFaces; faceIndex++)
    {
        uint32_t faceSize = (uint32_t)(faces->get()[faceIndex]);
        for (uint32_t i = 0; i < faceSize; ++i, ++facesIndicesCursor)
            indexesOfFacesValues[ facesIndices[facesIndicesCursor] ].push_back(facesIndicesCursor);
    }

    // 2nd, figure out which vertex can be merged, which cannot.
    // If all faces targetting a vertex give it the same normal and UV, then it can be shared.
    const abcV3 * normals = sample->m_smoothNormals.empty() && sample->m_normals.valid() ?  sample->m_normals.getVals()->get() : sample->m_smoothNormals.data();
    bool normalsIndexed = !sample->m_smoothNormals.empty() ? false : sample->m_normals.valid() && (sample->m_normals.getScope() == AbcGeom::kFacevaryingScope);
    const uint32_t *Nidxs = normalsIndexed ? sample->m_normals.getIndices()->get() : (uint32_t*)(topology->m_faceIndices->get());
    
    bool hasUVs = sample->m_uvs.valid();
    const auto *uvVals = sample->m_uvs.getVals()->get(); 
    const auto *uvIdxs = m_config.turnQuadEdges || !hasUVs ? topology->m_UvIndicesSwapedFaceWinding.data() : sample->m_uvs.getIndices()->get();

    topology->m_FixedTopoPositionsIndexes.clear();
    topology->m_FaceIndexingReindexed.clear();
    topology->m_FaceIndexingReindexed.resize(topology->m_faceIndices->size() );
    topology->m_FixedTopoPositionsIndexes.reserve(sample->m_positions->size());
    topology->m_FreshlyReadTopologyData = true;

    std::unordered_map< uint32_t, std::vector<uint32_t>>::iterator itr = indexesOfFacesValues.begin();
    while (itr != indexesOfFacesValues.end())
    {
        std::vector<uint32_t>& vertexUsages = itr->second;
        uint32_t vertexUsageIndex = 0;
        size_t vertexUsageMaxIndex = itr->second.size();
        const Abc::V2f * prevUV = nullptr;
        const abcV3 * prevN = nullptr;
        bool share = true;
        do
        {
            uint32_t index = vertexUsages[vertexUsageIndex];
            // same Normal?
            if( normals )
            {
                const abcV3 & N = normals[Nidxs ? Nidxs[index] : index];
                if (prevN == nullptr)
                    prevN = &N;
                else
                    share = N == *prevN;
            }
            // Same UV?
            if (hasUVs)
            {
                const Abc::V2f & uv = uvVals[uvIdxs[index]];
                if (prevUV == nullptr)
                    prevUV = &uv;
                else
                    share = uv == *prevUV;
            }
        }
        while (share && ++vertexUsageIndex < vertexUsageMaxIndex);

        // Verdict is in for this vertex.
        if (share)
            topology->m_FixedTopoPositionsIndexes.push_back(itr->first);

        auto indexItr = itr->second.begin();
        while( indexItr != itr->second.end() )
        {
            if (!share)
                topology->m_FixedTopoPositionsIndexes.push_back(itr->first);

            topology->m_FaceIndexingReindexed[*indexItr] = (uint32_t)topology->m_FixedTopoPositionsIndexes.size() - 1;

            ++indexItr;
        }
        ++itr;
    }

    // We now have a lookup for face value indexes that re-routes to shared indexes when possible!
    // Splitting does not work with shared vertices, if the resulting mesh still exceeds the splitting threshold, then disable vertex sharing.
    const int maxVertexSplitCount = topology->m_use32BitsIndexBuffer ? MAX_VERTEX_SPLIT_COUNT_32 : MAX_VERTEX_SPLIT_COUNT_16;
    if(topology->m_FixedTopoPositionsIndexes.size() / maxVertexSplitCount>0)
    {
        topology->m_vertexSharingEnabled = false;
        topology->m_FixedTopoPositionsIndexes.clear();
        topology->m_FaceIndexingReindexed.clear();
    }
}

void aiPolyMesh::updatePeakIndexCount() const
{
    if (m_peakIndexCount != 0) { return; }

    DebugLog("aiPolyMesh::updateMaxIndex()");

    Util::Dimensions dim;
    Abc::Int32ArraySamplePtr counts;

    auto indicesProp = m_schema.getFaceIndicesProperty();
    auto countsProp = m_schema.getFaceCountsProperty();

    int numSamples = static_cast<int>(indicesProp.getNumSamples());
    if (numSamples == 0) { return; }

    size_t cMax = 0;
    if (indicesProp.isConstant())
    {
        auto ss = Abc::ISampleSelector(int64_t(0));
        countsProp.get(counts, ss);
        indicesProp.getDimensions(dim, ss);
        cMax = dim.numPoints();
    }
    else
    {
        DebugLog("Checking %d sample(s)", numSamples);
        int iMax = 0;

        for (int i = 0; i < numSamples; ++i) {
            indicesProp.getDimensions(dim, Abc::ISampleSelector(int64_t(i)));
            size_t numIndices = dim.numPoints();
            if (numIndices > cMax) {
                cMax = numIndices;
                iMax = i;
            }
        }

        countsProp.get(counts, Abc::ISampleSelector(int64_t(iMax)));
    }

    m_peakIndexCount = (int)cMax;
    m_peakTriangulatedIndexCount = CalculateTriangulatedIndexCount(*counts);
}

int aiPolyMesh::getPeakIndexCount() const
{
    updatePeakIndexCount();
    return m_peakTriangulatedIndexCount;
}

int aiPolyMesh::getPeakTriangulatedIndexCount() const
{
    updatePeakIndexCount();
    return m_peakTriangulatedIndexCount;
}

int aiPolyMesh::getPeakVertexCount() const
{
    if (m_peakVertexCount == 0)
    {
        DebugLog("aiPolyMesh::getPeakVertexCount()");
        
        Util::Dimensions dim;

        auto positionsProp = m_schema.getPositionsProperty();

        int numSamples = (int)positionsProp.getNumSamples();

        if (numSamples == 0)
        {
            return 0;
        }
        else if (positionsProp.isConstant())
        {
            positionsProp.getDimensions(dim, Abc::ISampleSelector(int64_t(0)));
            
            m_peakVertexCount = (int)dim.numPoints();
        }
        else
        {
            m_peakVertexCount = 0;

            for (int i = 0; i < numSamples; ++i)
            {
                positionsProp.getDimensions(dim, Abc::ISampleSelector(int64_t(i)));
                
                size_t numVertices = dim.numPoints();

                if (numVertices > size_t(m_peakVertexCount))
                {
                    m_peakVertexCount = int(numVertices);
                }
            }
        }
    }

    return m_peakVertexCount;
}

void aiPolyMesh::getSummary(aiMeshSummary &summary) const
{
    DebugLog("aiPolyMesh::getSummary()");
    
    summary.topologyVariance = static_cast<int>(m_schema.getTopologyVariance());
    summary.peakVertexCount = getPeakVertexCount();
    summary.peakIndexCount = getPeakIndexCount();
    summary.peakTriangulatedIndexCount = getPeakTriangulatedIndexCount();
    summary.peakSubmeshCount = ceildiv(summary.peakIndexCount, 64998);
}
