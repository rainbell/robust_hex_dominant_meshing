#include "hierarchy.h"
#include "positions.h"
#include "timer.h"

void MultiResolutionHierarchy::smoothPositionsTri(uint32_t l, bool alignment, bool randomization, bool extrinsic) {
    const SMatrix &L = mL[l];
    const MatrixXf &V = mV[l], &N = mN[l], &Q = mQ[l];
    MatrixXf &O = mO[l];

    Timer<> timer;
   // timer.beginStage("Smoothing orientations at level " + std::to_string(l));

    double error = 0;
    int nLinks = 0;
    MatrixXf O_new(O.rows(), O.cols());
    tbb::spin_mutex mutex;

    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0u, (uint32_t) L.outerSize(), GRAIN_SIZE),
        [&](const tbb::blocked_range<uint32_t> &range) {
            std::vector<std::pair<uint32_t, Float>> neighbors;
            double errorLocal = 0;
            int nLinksLocal = 0;
            for (uint32_t k = range.begin(); k != range.end(); ++k) {
                SMatrix::InnerIterator it(L, k);

                uint32_t i = it.row();

                const Vector3f q_i = Q.col(i);
                const Vector3f n_i = N.col(i), v_i = V.col(i);
                Vector3f o_i = O.col(i);

                neighbors.clear();
                for (; it; ++it) {
                    uint32_t j = it.col();
                    if (i == j)
                        continue;
                    neighbors.push_back(std::make_pair(j, it.value()));
                }

                if (randomization && neighbors.size() > 0)
                    pcg32(mPositionIterations, k)
                        .shuffle(neighbors.begin(), neighbors.end());

                Float weightSum = 0.f;
                for (auto n : neighbors) {
                    uint32_t j = n.first;
                    Float value = n.second;

                    const Vector3f q_j = Q.col(j), v_j = V.col(j), n_j = N.col(j);
                    Vector3f o_j = O.col(j);

                    if (extrinsic) {
                        errorLocal += (O.col(i) -
                             findClosestPairExtrinsic(O.col(i), q_i, n_i, v_i, o_j, q_j, n_j,
                                             v_j, mScale, mInvScale)).norm();
                        o_j = findClosestPairExtrinsic(o_i, q_i, n_i, v_i, o_j, q_j, n_j,
                                              v_j, mScale, mInvScale);
                    } else {
                        errorLocal += (O.col(i) -
                             findClosestPair(O.col(i), q_i, n_i, v_i, o_j, q_j, n_j,
                                             v_j, mScale, mInvScale).first).norm();
                        o_j = findClosestPair(o_i, q_i, n_i, v_i, o_j, q_j, n_j,
                                              v_j, mScale, mInvScale).first;
                    }

                    o_i = value * o_j + weightSum * o_i;
                    weightSum += value;
                    o_i /= weightSum;
                    nLinksLocal++;
                }

                o_i = findClosest(o_i, q_i, n_i, v_i, mScale, mInvScale);
                o_i -= n_i.dot(o_i - v_i) * n_i;

				//if(l ==0 && nV_boundary_flag[l][i])
				if (nV_boundary_flag[l][i])
					o_i = q_i.dot(o_i - v_i) * q_i + v_i;
                
				O_new.col(i) = o_i;
            }
            tbb::spin_mutex::scoped_lock guard(mutex);
            error += errorLocal;
            nLinks += nLinksLocal;
        }
    );
    //timer.endStage("E = " + std::to_string(error / nLinks));
    mOrientationIterations++;
    O = std::move(O_new);
}

void MultiResolutionHierarchy::smoothPositionsTriCombed() {
    auto const &Q = mQ_combed, &N = mN[0], &V = mV[0];
    auto const &Oi = mO_combed;
    auto const &F = mF;
    auto &O = mO[0];

    MatrixXf O_new;
    VectorXi count;
    O_new.setZero(O.rows(), O.cols());
    count.setZero(O.cols());

    for (uint32_t f = 0; f < mF.cols(); ++f) {
        for (uint32_t i =0; i<3; ++i) {
            uint32_t v0 = F(i, f), v1 = F((i + 1) % 3, f);
            typedef Eigen::Matrix<Float, 3, 2> Matrix;

            const Vector3f &q0 = Q.col(3 * f + i);
            const Vector3f &q1_ = Q.col(3 * f + (i + 1) % 3);
            const Vector2i &t0 = Oi.col(3 * f + i);
            const Vector3f &o1_ = O.col(v1);
            const Vector3f &n0 = N.col(v0), &n1 = N.col(v1);
            const Vector3f &p0 = V.col(v0), &p1 = V.col(v1);

            Vector3f q1 = rotateVectorIntoPlane(q1_, n1, n0);
            Vector3f qn = (q0 + q1).normalized();
            Vector3f middle = middle_point(p0, n0, p1, n1);
            Vector3f o1 = rotateVectorIntoPlane(o1_ - middle, n1, n0) + middle;
            Matrix M = (Matrix() << qn, n0.cross(qn)).finished();
            o1 += (M * t0.cast<Float>()) * mScale;
            o1 -= n0.dot(o1 - p0) * n0;

            O_new.col(v0) += o1;
            count[v0]++;
        }
    }
    for (int i=0; i<O.cols(); ++i) {
        if (count[i] > 0)
            O_new.col(i) /= count[i];
    }
    O = std::move(O_new);
}

void MultiResolutionHierarchy::smoothPositionsTet(uint32_t l, bool alignment, bool randomization) {
    const SMatrix &L = mL[l];
    const MatrixXf &V = mV[l], &N = mN[l], &Q = mQ[l], &C = mC[l];
    MatrixXf &O = mO[l];

    Timer<> timer;
    //timer.beginStage("Smoothing orientations at level " + std::to_string(l));

    double error = 0;
    int nLinks = 0;
    MatrixXf O_new(O.rows(), O.cols());
    tbb::spin_mutex mutex;

#if 1
    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0u, (uint32_t) L.outerSize(), GRAIN_SIZE),
        [&](const tbb::blocked_range<uint32_t> &range) {
            std::vector<std::pair<uint32_t, Float>> neighbors;
            double errorLocal = 0;
            int nLinksLocal = 0;
            for (uint32_t k = range.begin(); k != range.end(); ++k) {
#endif
				//std::vector<std::pair<uint32_t, Float>> neighbors;
				//double errorLocal = 0;
				//int nLinksLocal = 0;
				//for (uint32_t k = 0; k <L.outerSize(); ++k) {
				//	
					SMatrix::InnerIterator it(L, k);

                uint32_t i = it.row();

                const Quaternion q_i = Q.col(i);
                const Vector3f n_i = N.col(i), v_i = V.col(i), c_i = C.col(i);
                Vector3f o_i = O.col(i);

                neighbors.clear();
                for (; it; ++it) {
                    uint32_t j = it.col();
                    if (i == j)
                        continue;
                    neighbors.push_back(std::make_pair(j, it.value()));
                }

                if (randomization && neighbors.size() > 0)
                    pcg32(mPositionIterations, k)
                        .shuffle(neighbors.begin(), neighbors.end());

                Float weightSum = 0.f;
                for (auto n : neighbors) {
                    uint32_t j = n.first;
                    Float value = n.second;

                    const Quaternion q_j = Q.col(j);
                    Vector3f o_j = O.col(j);

                    errorLocal += (O.col(i) - findClosestPair(O.col(i), q_i, o_j, q_j, mScale, mInvScale).first).norm();
                    nLinksLocal++;

                    o_j = findClosestPair(o_i, q_i, o_j, q_j, mScale, mInvScale).first;
                    o_i = value * o_j + weightSum * o_i;
                    weightSum += value;
                    o_i /= weightSum;

                    if (alignment && n_i != Vector3f::Zero()) {
                        Float dp = n_i.dot(c_i - o_i) * mInvScale;
                        o_i += (dp - std::round(dp)) * n_i * mScale;  
					}
					//if (alignment && n_i != Vector3f::Zero())
					//	o_i -= n_i.dot(o_i - v_i) * n_i;
                }

				o_i = findClosest(o_i, q_i, v_i, mScale, mInvScale);

				//if (l == 200000 && n_i != Vector3f::Zero()) {
				//	Vector3d v = o_i.cast<double>();
				//	//cout << "phone" << endl;
				//	Vector3d interpolP, interpolN;
				//	vector<uint32_t>  tids = vnfs[i];

				//	//tbb::spin_mutex::scoped_lock guard(mutex);
				//	//if (phong_projection(tids, vnfs, v, interpolP, interpolN)) {
				//		//o_i = interpolP.cast<Float>();
				//		//cout << "projected" << endl;
				//	//}

				//}
				//if (n_i != Vector3f::Zero())
				//	o_i -= n_i.dot(o_i - v_i) * n_i;
				O_new.col(i) = o_i;

			}
            tbb::spin_mutex::scoped_lock guard(mutex);
            error += errorLocal;
            nLinks += nLinksLocal;

#if 1
        }
    );
#endif

    //timer.endStage("E = " + std::to_string(error / nLinks));
    mOrientationIterations++;
    O = std::move(O_new);
}


void MultiResolutionHierarchy::prolongPositions(int level) {
    const SMatrix &P = mP[level];

    for (int k = 0; k < P.outerSize(); ++k) {
        SMatrix::InnerIterator it(P, k);
        for (; it; ++it)
            mO[level].col(it.row()) = mO[level+1].col(it.col());
    }
}

void MultiResolutionHierarchy::detectPositionSingularitiesTri() {
    Timer<> timer;
    timer.beginStage("Computing position singularities");
    const MatrixXu &F = mF;
    const MatrixXf &V = mV[0], &O = mO[0], &N = mN[0], &NF = mNF, &Q = mQ[0];
    MatrixXf &S = mPositionSingularities;
    uint32_t singularityCount = 0;
    std::mutex mutex;

    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0u, (uint32_t) F.cols(), 1000),
        [&](const tbb::blocked_range<uint32_t> &range) {
            for (uint32_t f = range.begin(); f != range.end(); ++f) {
                uint32_t k[3] = { F(0, f), F(1, f), F(2, f) };
                Vector2i trans = Vector2i::Zero();
                Vector3f q_cur = Q.col(k[0]);
                Vector3f face_center = Vector3f::Zero();

                for (int j = 0; j < 3; ++j) {
                    int n = (j + 1) % 3;
                    Vector3f q_next = applyRotationKeep(q_cur, N.col(k[j]), Q.col(k[n]), N.col(k[n]));
                    trans += findClosestPair(O.col(k[j]), q_cur,  N.col(k[j]), V.col(k[j]),
                                                O.col(k[n]), q_next, N.col(k[n]), V.col(k[n]),
                                                mScale, mInvScale).second;
                    q_cur = q_next;
                    face_center += V.col(k[j]);
                }

                if (std::abs(q_cur.dot(Q.col(k[0])) - 1) > 1e-3)
                    continue;

                if (trans != Vector2i::Zero()) {
                    face_center *= 1.f / 3.f;
                    std::lock_guard<std::mutex> lock(mutex);

                    if (singularityCount + 1 > S.cols())
                        S.conservativeResize(9, S.cols() * 2 + 1);
                    S.col(singularityCount++)
                        << face_center + NF.col(f) * mAverageEdgeLength / 3, NF.col(f), Vector3f(1, 1, 0);
                }
            }
        }
    );
    S.conservativeResize(9, singularityCount);
    timer.endStage("Found " + std::to_string(singularityCount) + " singular faces");
}

void MultiResolutionHierarchy::detectPositionSingularitiesTet() {
    Timer<> timer;
    timer.beginStage("Computing position singularities");

	//projectBack3D();

    const MatrixXu &T = mT;
    const MatrixXf &V = mV[0], &O = mO[0], &Q = mQ[0];
    MatrixXf &S = mPositionSingularities;
    uint32_t singularityCount = 0;
    std::mutex mutex;
    uint8_t tet_faces[4][3] = { { 1, 0, 2 }, { 3, 2, 0 }, { 1, 2, 3 }, { 0, 1, 3 } };

    tbb::parallel_for(
        tbb::blocked_range<uint32_t>(0u, (uint32_t) T.cols(), 1000),
        [&](const tbb::blocked_range<uint32_t> &range) {
            for (uint32_t t = range.begin(); t != range.end(); ++t) {

                for (auto f_: tet_faces) {
                    uint32_t f[3] = { T(f_[0], t), T(f_[1], t), T(f_[2], t) };

                    Vector3i trans = Vector3i::Zero();
                    Quaternion q_cur = Q.col(f[0]);
                    for (int j = 0; j < 3; ++j) {
                        int n = (j + 1) % 3;
                        Quaternion q_next = Quaternion::applyRotation(Q.col(f[n]), q_cur);
                        trans += findClosestPair(O.col(f[j]), q_cur, O.col(f[n]),
                                             q_next, mScale, mInvScale).second;
                        q_cur = q_next;
                    }
                    if (std::abs(q_cur.dot(Q.col(f[0])) - 1) > 1e-3)
                        continue;

                    if (trans != Vector3i::Zero()) {
                        std::lock_guard<std::mutex> lock(mutex);

                        Vector3f tc =
                            0.25f * (V.col(T(0, t)) + V.col(T(1, t)) +
                                     V.col(T(2, t)) + V.col(T(3, t)));

                        Vector3f fc = (V.col(f[0]) + V.col(f[1]) +
                                       V.col(f[2])) * (1.f / 3.f);

                        if (singularityCount + 2 > S.cols())
                            S.conservativeResize(6, 2*S.cols() + 2);
                        Vector3f color(1,1,0);
                        S.col(singularityCount++) << tc, color;
                        S.col(singularityCount++) << fc, color;
                    }
                }
            }
        }
    );
    S.conservativeResize(6, singularityCount);
    timer.endStage("Found " + std::to_string(singularityCount) + " singular faces");

	char path[1024], path_[1024];
	strcpy(path_, outpath.c_str());
	strncpy(path_, outpath.c_str(), sizeof(path_));
	path_[sizeof(path_) - 1] = 0;
	sprintf(path, "%s%s", path_, "_Posy.sing");
	write_singularities_SING(S, path);
}