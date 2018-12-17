
// #define ARMA_NO_DEBUG

#include <RcppArmadillo.h>
#include <progress.hpp>
#include <progress_bar.hpp>
#include <random>

#include "sim.hpp"
#include "pcg.hpp"
#include "quant_gen.hpp"

#ifdef _OPENMP
#include <omp.h>  // omp
#endif


using namespace Rcpp;



/*
 Note that this is the "instantaneous" version of quantitative genetics.
 It's not the same as what's written in Tobin and Tony's methods section.
 Instead of adding new daughter species, it starts with a number of species
 that should be more than you would ever expect to be produced sequentially.
 */




//' Derivative of fitness with respect to the trait divided by mean fitness.
//'
//' The function below calculates this selection strength for all traits
//' for all species.
//'
//' @noRd
//'
void sel_str__(arma::mat& ss_mat,
               const std::vector<arma::rowvec>& V,
               const std::vector<double>& N,
               const double& f,
               const double& g,
               const arma::mat& C,
               const double& r0,
               const double& d) {

    uint32_t n_spp = V.size();      // # species
    uint32_t n_trt = V[0].size();   // # traits

    if (ss_mat.n_rows != n_spp || ss_mat.n_cols != n_trt) ss_mat.set_size(n_spp, n_trt);

    // For all i, calculate `N_j * exp(-d * V_j * transpose(V_j))`, where `j != i`
    arma::vec W(n_spp, arma::fill::zeros);
    for (uint32_t j = 0; j < n_spp; j++) {
        // Calculate `N_j * exp(-d * V_j * transpose(V_j))`:
        double W_ = N[j] * std::exp(-d * arma::as_scalar(V[j] * V[j].t()));
        // Now insert this value at all `i` where `j != i`:
        for (uint32_t i = 0; i < n_spp; i++) {
            if (i == j) continue;
            W(i) += W_;
        }
    }
    arma::mat C_ = C + C.t();

    // Now go back through and calculate strength of selection:
    for (uint32_t i = 0; i < n_spp; i++) {
        const arma::rowvec& V_i(V[i]);
        const double& N_i(N[i]);
        ss_mat.row(i) = (-f * V_i * C_) +
            2 * g * V_i * std::exp(arma::as_scalar(-V_i * V_i.t())) * (N_i + W(i));
    }

    return;

}


//' R-exported version of above, so it can be tested in R for accuracy.
//'
//' @noRd
//'
//[[Rcpp::export]]
arma::mat sel_str_cpp(const std::vector<arma::rowvec>& V,
                      const std::vector<double>& N,
                      const double& f,
                      const double& g,
                      const arma::mat& C,
                      const double& r0,
                      const double& d) {

    arma::mat ss_mat;

    sel_str__(ss_mat, V, N, f, g, C, r0, d);

    return ss_mat;
}


//' Partial derivative of species i traits at time t+1 with respect to species i traits
//' at time t.
//'
//'
//' @noRd
//'
void dVi_dVi_(arma::mat& dVhat,
              const uint32_t& row_start,
              const uint32_t& col_start,
              const arma::rowvec& Vi, const double& Z,
              const arma::mat& CCC, const double& f, const double& g,
              const double& sigma2) {
    uint32_t q = Vi.n_elem;
    arma::mat I = arma::eye<arma::mat>(q, q);
    uint32_t row_end = row_start + q - 1;
    uint32_t col_end = col_start + q - 1;
    dVhat(arma::span(row_start, row_end), arma::span(col_start, col_end)) = I + sigma2 * (
        (
                2 * g * Z * std::exp(-1 * arma::as_scalar(Vi * Vi.t())) *
                    (I - 2 * (Vi.t() * Vi))
        ) - (f * CCC)
    );
    return;
}


//' R-exported version of above, to be used in R for testing.
//'
//' @noRd
//'
//[[Rcpp::export]]
arma::mat dVi_dVi_cpp(const uint32_t& i, const arma::mat& V, const double& Z,
                      const arma::mat& CCC, const double& f, const double& g,
                      const double& sigma2) {
    uint32_t q = V.n_cols;
    arma::mat dVhat(q, q);

    // Fill dVhat:
    dVi_dVi_(dVhat, 0, 0, V.row(i), Z, CCC, f, g, sigma2);

    return dVhat;
}


//' Partial derivative of species i traits at time t+1 with respect to species k traits
//' at time t.
//'
//' @noRd
//'
//'
void dVi_dVk_(arma::mat& dVhat,
              const uint32_t& row_start,
              const uint32_t& col_start,
              const double& Nk,
              const arma::rowvec& Vi,
              const arma::rowvec& Vk,
              const double& d, const double& g,
              const double& sigma2) {
    uint32_t row_end = row_start + Vi.n_elem - 1;
    uint32_t col_end = col_start + Vi.n_elem - 1;
    dVhat(arma::span(row_start, row_end), arma::span(col_start, col_end)) =
        -4 * sigma2 * Nk * d * g * (
                ( Vk.t() * arma::exp(-d * Vk * Vk.t()) ) *
                ( arma::exp(-1 * Vi * Vi.t()) * Vi )
        );
    return;
}


//' R-exported version of above, to be used in R for testing.
//'
//' @noRd
//'
//[[Rcpp::export]]
arma::mat dVi_dVk_cpp(const uint32_t& i, const uint32_t& k,
                      const std::vector<double>& N, const arma::mat& V,
                      const double& d, const double& g,
                      const double& sigma2) {
    uint32_t q = V.n_cols;
    arma::mat dVhat(q, q);
    // Fill dVhat:
    dVi_dVk_(dVhat, 0, 0, N[k], V.row(i), V.row(k), d, g, sigma2);

    return dVhat;
}











//' Compute Hessian matrices for a particular set of species' traits.
//'
//' @noRd
//'
//'
void hessian_(arma::cube& hessian,
              const std::vector<arma::rowvec>& V,
              const std::vector<double>& N,
              const double& f,
              const double& g,
              const arma::mat& C,
              const double& r0,
              const double& d,
              const arma::vec& add_var,
              const double& eps) {

    uint32_t n = N.size();
    uint32_t q = V[0].n_elem;
    arma::mat eye_(q, q, arma::fill::eye);

    if (hessian.n_rows != q || hessian.n_cols != q || hessian.n_slices != n) {
        hessian.set_size(q, q, n);
    }

    arma::mat ss_mat;
    // Fill strength of selection matrix:
    sel_str__(ss_mat, V, N, f, g, C, r0, d);
    // Trait change based on current trait values:
    std::vector<arma::rowvec> FV(n, arma::rowvec(q));
    for (uint32_t i = 0; i < n; i++) FV[i] = V[i] + add_var(i) * ss_mat.row(i);

    // Version of traits to manipulate:
    std::vector<arma::rowvec> V_m = V;
    // Trait change based on manipulated trait values:
    arma::rowvec FV_m(q);

    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < q; j++) {
            V_m[i](j) = V[i](j) + eps;
            sel_str__(ss_mat, V_m, N, f, g, C, r0, d);
            FV_m = V_m[i] + add_var(i) * ss_mat.row(i);
            hessian.slice(i).row(j) = (FV_m - FV[i]) / eps;
            V_m[i](j) = V[i](j);
        }
        hessian.slice(i) -= eye_;
    }

    return;
}



//' R-exported version of above, so it can be tested in R for accuracy.
//'
//' @noRd
//'
//[[Rcpp::export]]
arma::cube hessian_cpp(const std::vector<arma::rowvec>& V,
                       const std::vector<double>& N,
                       const double& f,
                       const double& g,
                       const arma::mat& C,
                       const double& r0,
                       const double& d,
                       const arma::vec& add_var,
                       const double& eps) {

    if (V.size() != N.size()) stop("V.size() != N.size()");
    if (add_var.n_elem != N.size()) stop("add_var.n_elem != N.size()");

    arma::cube hessian;

    hessian_(hessian, V, N, f, g, C, r0, d, add_var, eps);

    return hessian;
}





//' Creat Jacobian matrix for a particular set of species' traits.
//'
//'
//' @noRd
//'
void jacobian_(arma::mat& jacobian,
               const std::vector<arma::rowvec>& V,
               const std::vector<double>& N,
               const double& f,
               const double& g,
               const arma::mat& C,
               const double& r0,
               const double& d,
               const arma::vec& add_var,
               const double& eps) {

    uint32_t n = N.size();
    uint32_t q = V[0].n_elem;
    uint32_t nq = n * q;

    if (jacobian.n_rows != nq || jacobian.n_cols != nq) {
        jacobian.set_size(nq, nq);
    }

    arma::mat ss_mat;
    // Fill strength of selection matrix:
    sel_str__(ss_mat, V, N, f, g, C, r0, d);
    // Trait change based on current trait values:
    arma::rowvec FV(nq);
    for (uint32_t i = 0; i < n; i++) {
        FV(arma::span(i*q, (i+1)*q-1)) = V[i] + add_var(i) * ss_mat.row(i);
    }

    // Version of traits to manipulate:
    std::vector<arma::rowvec> V_m = V;
    // Trait change based on manipulated trait values:
    arma::rowvec FV_m(nq);

    uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = 0; j < q; j++) {
            V_m[i](j) = V[i](j) + eps;
            sel_str__(ss_mat, V_m, N, f, g, C, r0, d);
            for (uint32_t ii = 0; ii < n; ii++) {
                FV_m(arma::span(ii*q, (ii+1)*q-1)) = V_m[ii] +
                    add_var(ii) * ss_mat.row(ii);
            }
            jacobian.row(k) = (FV_m - FV) / eps;
            V_m[i](j) = V[i](j);
            k++;
        }
    }

    return;

}


//' R-exported version of above, so it can be tested in R for accuracy.
//'
//' @noRd
//'
//[[Rcpp::export]]
arma::mat jacobian_cpp(const std::vector<arma::rowvec>& V,
                        const std::vector<double>& N,
                        const double& f,
                        const double& g,
                        const arma::mat& C,
                        const double& r0,
                        const double& d,
                        const arma::vec& add_var,
                        const double& eps) {

    if (V.size() != N.size()) stop("V.size() != N.size()");
    if (add_var.n_elem != N.size()) stop("add_var.n_elem != N.size()");

    arma::mat jacobian;

    jacobian_(jacobian, V, N, f, g, C, r0, d, add_var, eps);

    return jacobian;
}





//' Search for unique species in a matrix of species trait values.
//'
//' @noRd
//'
arma::uvec unq_spp_(const std::vector<arma::rowvec>& V,
                    const std::vector<double>& N,
                    const double& precision) {

    double precision_ = precision * precision;

    uint32_t n = V.size();
    arma::uvec is_unq(n, arma::fill::ones);
    for (uint32_t i = 1; i < n; i++) {
        for (uint32_t j = 0; j < i; j++) {
            if (is_unq(j) == 0) continue; // don't want to keep looking at non-unique spp
            arma::rowvec diff_;
            diff_ = V[i] - V[j];
            diff_ %= diff_;
            if (arma::mean(diff_) < precision_) {
                is_unq(i) = 0;
                break;
            }
        }
    }

    arma::uvec unqs = arma::find(is_unq == 1);

    return unqs;
}






//' One repetition of quantitative genetics.
//'
//' Higher-up function(s) should handle the info put into `info`.
//'
//'
//' @noRd
//'
void one_quant_gen__(OneRepInfo& info,
                     const std::vector<arma::rowvec>& V0,
                     const std::vector<double>& N0,
                     const double& f,
                     const double& g,
                     const double& eta,
                     const double& r0,
                     const double& d,
                     const arma::vec& add_var,
                     const double& mut_sd,
                     const bool& keep_pos,
                     const uint32_t& start_t,
                     const uint32_t& max_t,
                     const double& min_N,
                     const uint32_t& save_every,
                     const bool& rm_extinct,
                     pcg64& eng) {


    info = OneRepInfo(N0, V0, max_t, save_every, keep_pos, mut_sd);

    arma::mat C(V0[0].n_elem, V0[0].n_elem);
    C.fill(eta);
    C.diag().fill(1.0);

    uint32_t t = 0;
    bool all_gone = false;

    while (!all_gone && t < start_t) {

        // Update abundances and traits:
        all_gone = info.iterate(f, g, C, r0, d, add_var, min_N, rm_extinct);
        t++;

    }

    // perturb trait values
    if (mut_sd > 0) info.perturb(eng);

    t = 0;
    while (!all_gone && t < max_t) {

        // Update abundances and traits:
        all_gone = info.iterate(f, g, C, r0, d, add_var, min_N, rm_extinct);

        if (save_every > 0 && (t % save_every == 0 || (t+1) == max_t || all_gone)) {
            info.save_time(t);
        }

        t++;
    }

    // Calculate final fitnesses and selection pressure to see if we're at equilibrium
    info.fitness_selection(f, g, C, r0, d);


    return;
}


//' Multiple repetitions of quantitative genetics.
//'
//'
//' @noRd
//'
//[[Rcpp::export]]
List quant_gen_cpp(const uint32_t& n_reps,
                  const std::vector<arma::rowvec>& V0,
                  const std::vector<double>& N0,
                  const double& f,
                  const double& g,
                  const double& eta,
                  const double& r0,
                  const double& d,
                  const arma::vec& add_var,
                  const double& mut_sd,
                  const bool& keep_pos,
                  const uint32_t& start_t,
                  const uint32_t& max_t,
                  const double& min_N,
                  const uint32_t& save_every,
                  const bool& show_progress,
                  const uint32_t& n_cores) {

    if (N0.size() != V0.size()) stop("N0.size() != V0.size()");
    if (add_var.n_elem != V0.size()) stop("add_var.n_elem != V0.size()");

    const bool rm_extinct(true);  // don't see a reason for it to ever be false
    std::vector<OneRepInfo> rep_infos(n_reps);

    const std::vector<std::vector<uint64_t>> seeds = mc_seeds(n_cores);
    Progress prog_bar(n_reps, show_progress);
    bool interrupted = false;

    #ifdef _OPENMP
    #pragma omp parallel default(shared) num_threads(n_cores) if (n_cores > 1)
    {
    #endif

    std::vector<uint64_t> active_seeds;

    // Write the active seed per core or just write one of the seeds.
    #ifdef _OPENMP
    uint32_t active_thread = omp_get_thread_num();
    #else
    uint32_t active_thread = 0;
    #endif
    active_seeds = seeds[active_thread];

    pcg64 eng = seeded_pcg(active_seeds);

    #ifdef _OPENMP
    #pragma omp for schedule(static)
    #endif
    for (uint32_t i = 0; i < n_reps; i++) {
        if (!Progress::check_abort()) {
            one_quant_gen__(rep_infos[i], V0, N0, f, g, eta, r0, d, add_var,
                            mut_sd, keep_pos, start_t, max_t, min_N, save_every,
                            rm_extinct, eng);
            prog_bar.increment();
        } else if (active_thread == 0) interrupted = true;
    }
    #ifdef _OPENMP
    }
    #endif

    if (interrupted) {
        throw(Rcpp::exception("\nUser interrupted process.", false));
    }

    /*
     ------------
     Now organize output:
     ------------
     */
    arma::mat nv; // N and V - either through time or just final values
    arma::mat fs(n_reps, 2); // fitness and selection
    uint32_t q = V0[0].n_cols;

    /*
     Go through one time to calculate the # surviving species for all reps and
     for all time point(s) saved.
     Then go back through and fill in values.
     */
    uint32_t total_n_spp = 0;
    if (save_every > 0) {  //   ---- Saving values through time: ----

        for (uint32_t i = 0; i < n_reps; i++) {
            for (uint32_t j = 0; j < rep_infos[i].t.size(); j++) {
                total_n_spp += rep_infos[i].N_t[j].size();
            }
        }
        nv.set_size(total_n_spp, 4 + q);

        uint32_t j = 0;
        for (uint32_t i = 0; i < n_reps; i++) {

            Rcpp::checkUserInterrupt();
            const OneRepInfo& info(rep_infos[i]);
            for (uint32_t t = 0; t < info.t.size(); t++) {

                const std::vector<double>& N_t(info.N_t[t]);
                const std::vector<arma::rowvec>& V_t(info.V_t[t]);
                const std::vector<uint32_t>& spp_t(info.spp_t[t]);
                const double& t_(info.t[t]);
                for (uint32_t k = 0; k < N_t.size(); k++) {

                    nv(j+k,0) = i;          // rep
                    nv(j+k,1) = t_;         // time
                    nv(j+k,2) = spp_t[k];   // species
                    nv(j+k,3) = N_t[k];     // N
                    // V:
                    for (uint32_t l = 0; l < q; l++) nv(j+k, 4+l) = V_t[k](l);

                }

                j += N_t.size();

            }

            fs(i,0) = info.fitness;
            fs(i,1) = info.selection;

        }

    } else {  //                ----  Just saving final values:  ----

        for (uint32_t i = 0; i < n_reps; i++) {
            total_n_spp += rep_infos[i].N.size();
        }
        nv.set_size(total_n_spp, 3 + q);
        uint32_t j = 0;
        for (uint32_t i = 0; i < n_reps; i++) {
            Rcpp::checkUserInterrupt();
            const OneRepInfo& info(rep_infos[i]);
            for (uint32_t k = 0; k < info.N.size(); k++) {
                nv(j+k,0) = i;          // rep
                nv(j+k,1) = k;          // species
                nv(j+k,2) = info.N[k];  // N
                // V:
                for (uint32_t l = 0; l < q; l++) nv(j+k, 3+l) = info.V[k](l);
            }
            j += info.N.size();
            fs(i,0) = info.fitness;
            fs(i,1) = info.selection;
        }

    }

    List out = List::create(_["NV"] = nv, _["FS"] = fs);

    return out;

}

