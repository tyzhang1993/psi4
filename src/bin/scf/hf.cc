/*
 *  hf.cpp
 *  matrix
 *
 *  Created by Justin Turney on 4/9/08.
 *  Copyright 2008 by Justin M. Turney, Ph.D.. All rights reserved.
 *
 */

#include <cstdlib>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include <utility>

#include <psifiles.h>
#include <libciomr/libciomr.h>
#include <libpsio/psio.hpp>
#include <libchkpt/chkpt.hpp>
#include <libipv1/ip_lib.h>
#include <libiwl/iwl.hpp>
#include <libqt/qt.h>

#include "hf.h"

#include <libmints/basisset.h>
#include <libmints/onebody.h>
#include <libmints/twobody.h>
#include <libmints/integral.h>
#include <libmints/molecule.h>

using namespace std;
using namespace psi;

namespace psi { namespace scf {
    
HF::HF(Options& options, shared_ptr<PSIO> psio, shared_ptr<Chkpt> chkpt) 
    : Wavefunction(options, psio, chkpt),
      nuclear_dipole_contribution_(3),
      nuclear_quadrupole_contribution_(6)
{
    common_init();
}

HF::~HF()
{
    delete[] so2symblk_;
    delete[] so2index_;
    delete[] pk_symoffset_;
    free(zvals_);    
}

void HF::common_init()
{
    S_.reset(factory_.create_matrix("S"));
    Shalf_.reset(factory_.create_matrix("S^-1/2"));
    Sphalf_.reset(factory_.create_matrix("S^+1/2"));
    H_.reset(factory_.create_matrix("One-electron Hamiltonion"));
    
    Eold_    = 0.0;
    E_       = 0.0;
    maxiter_ = 40;
    
    // Read information from input file
    maxiter_ = options_.get_int("MAXITER");
    
    // Read in DOCC and SOCC from memory
	int nirreps = factory_.nirreps();
	input_docc_ = false;
	if (options_["DOCC"].has_changed()) {
		input_docc_ = true;
		for (int i=0; i<nirreps; ++i)
            doccpi_[i] = options_["DOCC"][i].to_integer();
	} else {
		for (int i=0; i<nirreps; ++i)
			doccpi_[i] = 0;
	}
	input_socc_ = false;
	if (options_["SOCC"].has_changed()) {
		input_socc_ = true;
		for (int i=0; i<nirreps; ++i)
			soccpi_[i] = options_["SOCC"][i].to_integer();
	} else {
		for (int i=0; i<nirreps; ++i)
			soccpi_[i] = 0;
	}
	
	// TODO: Make the follow work for general cases!
	nalpha_ = nbeta_ = 0;
	for (int i=0; i<nirreps; ++i) {
		nalphapi_[i] = doccpi_[i] + soccpi_[i];
		nbetapi_[i]  = doccpi_[i];
		nalpha_ += doccpi_[i] + soccpi_[i];
		nbeta_  += doccpi_[i];
	}
	
    perturb_h_ = false;
    perturb_h_ = options_.get_bool("PERTURB_H");
    perturb_ = nothing;
    lambda_ = 0.0;
    if (perturb_h_) {
        string perturb_with;
        
        lambda_ = options_.get_double("LAMBDA");
        
        if (options_["PERTURB_WITH"].has_changed()) {
            perturb_with = options_.get_str("PERTURB_WITH");
            // Do checks to see what perturb_with is.
            if (perturb_with == "DIPOLE_X")
                perturb_ = dipole_x;
            else if (perturb_with == "DIPOLE_Y")
                perturb_ = dipole_y;
            else if (perturb_with == "DIPOLE_Z")
                perturb_ = dipole_z;
            else
                fprintf(outfile, "Unknown PERTURB_WITH. Applying no perturbation.\n");                
        } else {
            fprintf(outfile, "PERTURB_H is true, but PERTURB_WITH not found, applying no perturbation.\n");
        }
    }
	
    // Run integral direct? default no
	direct_integrals_ = false;
    direct_integrals_ = options_.get_bool("DIRECT");
    
    //Run density fitting? default no
    ri_integrals_ = false;
    
    if (options_["RI_BASIS"].has_changed())
    {
    	ri_integrals_ = true;
    	direct_integrals_ = false;
    }
    
    //Run schwarz sieve? default no
    schwarz_ = false;
    SCHWARZ_CUTOFF_ = 0.0;
    if (options_["SCHWARZ_CUTOFF"].has_changed()) {
      SCHWARZ_CUTOFF_ = options_.get_double("SCHWARZ_CUTOFF");
      schwarz_ = true;
    }
    
    
    // Read information from checkpoint
    nuclearrep_ = chkpt_->rd_enuc();
    natom_ = chkpt_->rd_natom();
    zvals_ = chkpt_->rd_zvals();
    
    // Alloc memory for multipoles
    Dipole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Dipole X SO-basis")));
    Dipole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Dipole Y SO-basis")));
    Dipole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Dipole Z SO-basis")));
    Quadrupole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Quadrupole XX")));
    Quadrupole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Quadrupole XY")));
    Quadrupole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Quadrupole XZ")));
    Quadrupole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Quadrupole YY")));
    Quadrupole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Quadrupole YZ")));
    Quadrupole_.push_back(SharedSimpleMatrix(factory_.create_simple_matrix("Quadrupole ZZ")));
    
    print_header();
    
    form_indexing();
}

void HF::print_header()
{
    char *temp;
    char **temp2;
    char *reference;
    
    ip_string(const_cast<char*>("REFERENCE"), &reference, 0);
    
    fprintf(outfile, " %s: by Justin Turney\n\n", reference);
#ifdef _DEBUG
    fprintf(outfile, "  Debug version.\n");
#else
    fprintf(outfile, "  Release version.\n");
#endif
    temp = chkpt_->rd_sym_label();
    fprintf(outfile, "  Running in %s symmetry.\n", temp);
    free(temp);
    
	temp2 = chkpt_->rd_irr_labs();
	fprintf(outfile, "  Input DOCC vector = (");
	for (int h=0; h<factory_.nirreps(); ++h) {
		fprintf(outfile, "%2d %3s ", doccpi_[h], temp2[h]);
	}
	fprintf(outfile, ")\n");
	fprintf(outfile, "  Input SOCC vector = (");
	for (int h=0; h<factory_.nirreps(); ++h) {
		fprintf(outfile, "%2d %3s ", soccpi_[h], temp2[h]);
		free(temp2[h]);
	}
	free(temp2);
	
    fprintf(outfile, ")\n");
    fprintf(outfile, "  Nuclear repulsion = %20.15f\n", nuclearrep_);
    
    fprintf(outfile, "  Energy threshold  = %3.2e\n", energy_threshold_);
    fprintf(outfile, "  Density threshold = %3.2e\n\n", density_threshold_);
    fflush(outfile);
    free(reference);
}

void HF::form_indexing()
{
    int h, i, ij, offset, pk_size;
    int nirreps = factory_.nirreps();
    int *opi = factory_.rowspi();
    int nso;
    
    nso = chkpt_->rd_nso();
    so2symblk_ = new int[nso];
    so2index_  = new int[nso];
    
    ij = 0; offset = 0; pk_size = 0; pk_pairs_ = 0;
    for (h=0; h<nirreps; ++h) {
        for (i=0; i<opi[h]; ++i) {
            so2symblk_[ij] = h;
            so2index_[ij] = ij-offset;
            
            if (debug_ > 3)
                fprintf(outfile, "_so2symblk[%3d] = %3d, _so2index[%3d] = %3d\n", ij, so2symblk_[ij], ij, so2index_[ij]);
            
            ij++;
        }
        offset += opi[h];
        
        // Add up possible pair combinations that yield A1 symmetry
        pk_pairs_ += ioff[opi[h]];
    }
    
    // Compute the number of pairs in PK
    pk_size_ = INDEX2(pk_pairs_-1, pk_pairs_-1) + 1;
    
    // Compute PK symmetry mapping
   	pk_symoffset_ = new int[nirreps];
    
   	// Compute an offset in the PK matrix telling where a given symmetry block starts.
    pk_symoffset_[0] = 0;
	for (h=1; h<nirreps; ++h) {
		pk_symoffset_[h] = pk_symoffset_[h-1] + ioff[opi[h-1]];
	}
}

void HF::form_H()
{
    SharedMatrix kinetic(factory_.create_matrix("Kinetic Integrals"));
    SharedMatrix potential(factory_.create_matrix("Potential Integrals"));
    
    // Form the multipole integrals
    form_multipole_integrals();
    
    // Load in kinetic and potential matrices
    int nso = chkpt_->rd_nso();
    double *integrals = init_array(ioff[nso]);
    
    // Kinetic
    if (!direct_integrals_) {
        IWL::read_one(psio_.get(), PSIF_OEI, const_cast<char*>(PSIF_SO_T), integrals, ioff[nso], 0, 0, outfile);
        kinetic->set(integrals);
        IWL::read_one(psio_.get(), PSIF_OEI, const_cast<char*>(PSIF_SO_V), integrals, ioff[nso], 0, 0, outfile);
        potential->set(integrals);
    }
    else {
        IntegralFactory integral(basisset_, basisset_, basisset_, basisset_);
        shared_ptr<OneBodyInt> T(integral.kinetic());
        shared_ptr<OneBodyInt> V(integral.potential());
        
        T->compute(kinetic);
        V->compute(potential);        
    }
    
    if (debug_ > 2)
        kinetic->print(outfile);
    
    if (debug_ > 2)
        potential->print(outfile);
    
    H_->copy(kinetic);
    H_->add(potential);
    
    if (debug_ > 2)
        H_->print(outfile);
    
    free(integrals);
    
    // if (perturb_h_) {
    //     if (perturb_ == dipole_x) {
    //         fprintf(outfile, "  Perturbing H by %f Dmx.\n", lambda_);
    //         H_.add(lambda_ * Dipole_[0]);
    //     } else if (perturb_ == dipole_y) {
    //         fprintf(outfile, "  Perturbing H by %f Dmy.\n", lambda_);
    //         H_.add(lambda_ * Dipole_[1]);
    //     } else if (perturb_ == dipole_z) {
    //         fprintf(outfile, "  Perturbing H by %f Dmz.\n", lambda_);
    //         H_.add(lambda_ * Dipole_[2]);
    //     }
    //     H_.print(outfile, "with perturbation");
    // }
}

void HF::form_Shalf()
{
    int nso = chkpt_->rd_nso();
    
    // Overlap
    if (!direct_integrals_) {
        double *integrals = init_array(ioff[nso]);
        IWL::read_one(psio_.get(), PSIF_OEI, const_cast<char*>(PSIF_SO_S), integrals, ioff[nso], 0, 0, outfile);
        S_->set(integrals);
        free(integrals);
    }
    else {
        IntegralFactory integral(basisset_, basisset_, basisset_, basisset_);
        OneBodyInt *S = integral.overlap();        
        S->compute(S_);
        delete S;
    }
    // Form S^(-1/2) matrix
    Matrix eigvec; 
    Matrix eigtemp;
    Matrix eigtemp2;
    Vector eigval;
    factory_.create_matrix(eigvec, "L");
    factory_.create_matrix(eigtemp, "Temp");
    factory_.create_matrix(eigtemp2);
    factory_.create_vector(eigval);
    
    S_->diagonalize(eigvec, eigval);    
    
    // Convert the eigenvales to 1/sqrt(eigenvalues)
    int *dimpi = eigval.dimpi();
    for (int h=0; h<eigval.nirreps(); ++h) {
        for (int i=0; i<dimpi[h]; ++i) {
            double scale = 1.0 / sqrt(eigval.get(h, i));
            eigval.set(h, i, scale);
        }
    }
    // Create a vector matrix from the converted eigenvalues
    eigtemp2.set(eigval);
    
    eigtemp.gemm(false, true, 1.0, eigtemp2, eigvec, 0.0);
    Shalf_->gemm(false, false, 1.0, eigvec, eigtemp, 0.0);
    
    // Convert the eigenvalues to sqrt(eigenvalues)
	for (int h=0; h<eigval.nirreps(); ++h) {
		for (int i=0; i<dimpi[h]; ++i) {
			double scale = sqrt(eigval.get(h, i));
			eigval.set(h, i, scale);
		}
	}
	// Create a vector matrix from the converted eigenvalues
	eigtemp2.set(eigval);
	
	// Works for diagonalize:
	eigtemp.gemm(false, true, 1.0, eigtemp2, eigvec, 0.0);
	Sphalf_->gemm(false, false, 1.0, eigvec, eigtemp, 0.0);
	
    if (debug_ > 3) {
        Shalf_->print(outfile);
        Sphalf_->print(outfile);
    }
}

int *HF::compute_fcpi(int nfzc, SharedVector eigvalues)
{
    int *frzcpi = new int[eigvalues->nirreps()];
    // Print out orbital energies.
    std::vector<std::pair<double, int> > pairs;
    for (int h=0; h<eigvalues->nirreps(); ++h) {
        for (int i=0; i<eigvalues->dimpi()[h]; ++i)
            pairs.push_back(make_pair(eigvalues->get(h, i), h));
        frzcpi[h] = 0;
    }
    sort(pairs.begin(),pairs.end());
    
    for (int i=0; i<nfzc; ++i)
        frzcpi[pairs[i].second]++;
    
    return frzcpi;
}

int *HF::compute_fvpi(int nfzv, SharedVector eigvalues)
{
    int *frzvpi = new int[eigvalues->nirreps()];
    // Print out orbital energies.
    std::vector<std::pair<double, int> > pairs;
    for (int h=0; h<eigvalues->nirreps(); ++h) {
        for (int i=0; i<eigvalues->dimpi()[h]; ++i)
            pairs.push_back(make_pair(eigvalues->get(h, i), h));
        frzvpi[h] = 0;
    }
    sort(pairs.begin(),pairs.end(), greater<std::pair<double, int> >());
    
    for (int i=0; i<nfzv; ++i)
        frzvpi[pairs[i].second]++;
    
    return frzvpi;
}

void HF::form_multipole_integrals()
{
    // Initialize an integral object
    IntegralFactory integral(basisset_, basisset_, basisset_, basisset_);
    
    // Get a dipole integral object
    OneBodyInt* dipole = integral.dipole();
    OneBodyInt* quadrupole= integral.quadrupole();
    
    // Compute the dipole integrals
    dipole->compute(Dipole_);
    quadrupole->compute(Quadrupole_);
    
    delete quadrupole;
    delete dipole;
    
    // Get the nuclear contribution to the dipole
    nuclear_dipole_contribution_ = molecule_->nuclear_dipole_contribution();
    nuclear_quadrupole_contribution_ = molecule_->nuclear_quadrupole_contribution();
        
    // Save the dipole integrals
    Dipole_[0]->save(psio_, PSIF_OEI);
    Dipole_[1]->save(psio_, PSIF_OEI);
    Dipole_[2]->save(psio_, PSIF_OEI);
    
    Quadrupole_[0]->save(psio_, PSIF_OEI);
    Quadrupole_[1]->save(psio_, PSIF_OEI);
    Quadrupole_[2]->save(psio_, PSIF_OEI);
    Quadrupole_[3]->save(psio_, PSIF_OEI);
    Quadrupole_[4]->save(psio_, PSIF_OEI);
    Quadrupole_[5]->save(psio_, PSIF_OEI);
}

void HF::form_B()
{   
    fprintf(outfile, "\n  Computing Integrals using RI Basis\n");
    if (factory_.nirreps() != 1)
    {
        fprintf(outfile,"Must run in C1 for now.\n");
        abort();
    } 
    int norbs = basisset_->nbf(); 
        
    shared_ptr<BasisSet> ribasis_ =shared_ptr<BasisSet>(new BasisSet(chkpt_, "DF_BASIS"));
    ri_nbf_ = ribasis_->nbf();
    //ribasis_->print();

    shared_ptr<BasisSet> zero = BasisSet::zero_basis_set();

    // Create integral factory
    IntegralFactory rifactory_J(ribasis_, zero, ribasis_, zero);
    TwoBodyInt* Jint = rifactory_J.eri();
    double **J = block_matrix(ri_nbf_, ri_nbf_);
    double **J_mhalf = block_matrix(ri_nbf_, ri_nbf_);
    const double *Jbuffer = Jint->buffer();

#ifdef TIME_SCF
    timer_init();
    timer_on("Form J");
#endif

    int index = 0;
  
    #ifdef OMP
    #pragma omp parallel for 
    #endif
    for (int MU=0; MU < ribasis_->nshell(); ++MU) {
        int nummu = ribasis_->shell(MU)->nfunction();
        
        for (int NU=0; NU < ribasis_->nshell(); ++NU) {
            int numnu = ribasis_->shell(NU)->nfunction();
    
            Jint->compute_shell(MU, 0, NU, 0);
            
            index = 0;
            for (int mu=0; mu < nummu; ++mu) {
                int omu = ribasis_->shell(MU)->function_index() + mu;
                
                for (int nu=0; nu < numnu; ++nu, ++index) {
                    int onu = ribasis_->shell(NU)->function_index() + nu;

                    J[omu][onu] = Jbuffer[index];
                }
            }
        }
    }
    //fprintf(outfile,"\nJ:\n");
    //print_mat(J,ri_nbf_,ri_nbf_,outfile);

    // Form J^-1/2
    // First, diagonalize J
    // the C_DSYEV call replaces the original matrix J with its eigenvectors
    double* eigval = init_array(ri_nbf_);
    int lwork = ri_nbf_ * 3;
    double* work = init_array(lwork);
    int stat = C_DSYEV('v','u',ri_nbf_,J[0],ri_nbf_,eigval, work,lwork);
    if (stat != 0) {
        fprintf(outfile, "C_DSYEV failed\n");
        exit(PSI_RETURN_FAILURE);
    }
    free(work);

    // Now J contains the eigenvectors of the original J
    // Copy J to J_copy
    double **J_copy = block_matrix(ri_nbf_, ri_nbf_);
    C_DCOPY(ri_nbf_*ri_nbf_,J[0],1,J_copy[0],1); 
  
    // Now form J^{-1/2} = U(T)*j^{-1/2}*U,
    // where j^{-1/2} is the diagonal matrix of the inverse square roots
    // of the eigenvalues, and U is the matrix of eigenvectors of J
    for (int i=0; i<ri_nbf_; i++) {
        if (eigval[i] < 1.0E-10)
            eigval[i] = 0.0;
        else 
            eigval[i] = 1.0 / sqrt(eigval[i]);
        
        // scale one set of eigenvectors by the diagonal elements j^{-1/2}
        C_DSCAL(ri_nbf_, eigval[i], J[i], 1);
    }
    free(eigval);

    // J_mhalf = J_copy(T) * J
    C_DGEMM('t','n',ri_nbf_,ri_nbf_,ri_nbf_,1.0,
    J_copy[0],ri_nbf_,J[0],ri_nbf_,0.0,J_mhalf[0],ri_nbf_);

    free_block(J);
    free_block(J_copy);
  
    //fprintf(outfile,"\nJmhalf:\n");
    //print_mat(J_mhalf,ri_nbf_,ri_nbf_,outfile);

#ifdef TIME_SCF
    timer_off("Form J");
#endif
#ifdef TIME_SCF
    timer_on("Form ao_p_ia");
#endif

    IntegralFactory rifactory(ribasis_, zero, basisset_, basisset_);
    TwoBodyInt* eri = rifactory.eri();
    const double *buffer = eri->buffer();
    double **ao_p_ia = block_matrix(ri_nbf_,basisset_->nbf()*(basisset_->nbf()+1)/2); 

    int numPshell,Pshell,MU,NU,P,PHI,mu,nu,nummu,numnu,omu,onu;
    #ifdef OMP
    #pragma omp parallel for private (numPshell,Pshell,MU,NU,P,PHI,mu,nu,nummu,numnu,omu,onu)
    #endif
    for (Pshell=0; Pshell < ribasis_->nshell(); ++Pshell) {
      numPshell = ribasis_->shell(Pshell)->nfunction();
      for (MU=0; MU < basisset_->nshell(); ++MU) {
        nummu = basisset_->shell(MU)->nfunction();
        for (NU=0; NU <= MU; ++NU) {
          numnu = basisset_->shell(NU)->nfunction();
          eri->compute_shell(Pshell, 0, MU, NU);
          for (P=0, index=0; P < numPshell; ++P) {
            PHI = ribasis_->shell(Pshell)->function_index() + P;
            for (mu=0; mu < nummu; ++mu) {
              omu = basisset_->shell(MU)->function_index() + mu;
              for (nu=0; nu < numnu; ++nu, ++index) {
                onu = basisset_->shell(NU)->function_index() + nu;
                if(omu>=onu)
                  ao_p_ia[PHI][ioff[omu]+onu]= buffer[index];
              } 
            }
          } // end loop over P in Pshell
        } // end loop over NU shell
      } // end loop over MU shell
      // now we've gone through all P, mu, nu for a given Pshell
    } // end loop over P shells; done with forming MO basis (P|ia)'s
  
    //fprintf(outfile,"\nao_p_ia:\n");
    //print_mat(ao_p_ia, ri_nbf_,norbs*(norbs+1)/2 ,outfile);

#ifdef TIME_SCF
    timer_off("Form ao_p_ia");
#endif
#ifdef TIME_SCF
    timer_on("Form B_ia^P");
#endif

    // ao_p_ia has integrals
    // B_ia^P = Sum_Q (i a | Q) (J^-1/2)_QP
    B_ia_P_ = block_matrix(ri_nbf_,norbs*(norbs+1)/2);

    C_DGEMM('N','N',ri_nbf_,norbs*(norbs+1)/2,ri_nbf_,
        1.0, J_mhalf[0], ri_nbf_, ao_p_ia[0], norbs*(norbs+1)/2,
        0.0, B_ia_P_[0], norbs*(norbs+1)/2);
    //fprintf(outfile,"\nB_p_ia:\n");
    //print_mat(B_ia_P_, ri_nbf_,norbs*(norbs+1)/2 ,outfile);
    free_block(ao_p_ia);
    free_block(J_mhalf);

#ifdef TIME_SCF
    timer_off("Form B_ia^P");
#endif 
}
void HF::schwarz_sieve()
{
		int* cut_ind_; //critical sorted index for schwarz sieve
    double* norm_;
    // Create integral object
    IntegralFactory integral(basisset_, basisset_, basisset_, basisset_);
    TwoBodyInt* eri = integral.eri();
    //ShellCombinationsIterator iter = integral.shells_iterator();
    const double *buffer = eri->buffer();
    
    fprintf(outfile, "\n      Sieving..."); fflush(outfile);
    //Superdiagonal computation
    int nshell = basisset_->nshell();
    norm_ = init_array(nshell*(nshell+1)/2);
    sieve_ind_ = new int[nshell*(nshell+1)/2];
    #ifdef OMP
    #pragma omp parallel for
    #endif
    for (int i = 0, ij = 0; i<nshell; i++) {
        for (int j = 0; j<=i; j++, ij++)
        {
            eri->compute_shell(i, j, i, j);
            double mnorm = 0.0;
            int index = 0;
            int NI = basisset_->shell(i)->nfunction();
            int NJ = basisset_->shell(j)->nfunction();
            for (int ii = 0; ii<NI; ii++)
                for (int jj = 0; jj<NJ; jj++)
                    for (int k = 0; k<NI; k++)
                        for (int l = 0; l<NJ; l++, index++)
                            if (fabs(buffer[index])>mnorm)
                                mnorm = fabs(buffer[index]);
            norm_[ij] = mnorm;
            sieve_ind_[ij] = ij;
        }
    }
#ifdef DEBUG
    fprintf(outfile,"\nUnsorted Vals:\n"); fflush(outfile);
    for (int i = 0; i<nshell*(nshell+1)/2; i++)
        fprintf(outfile,"\t%d\t%d\t%f\n",i,sieve_ind_[i],norm[i]);
    fflush(outfile);
#endif
    
    //Quicksort by norm
#ifdef DEBUG
    fprintf(outfile, "\n      Quicksort..."); fflush(outfile);
#endif
    //In-place quicksort, max elements = 2^MAX_LEVELS
    #define  MAX_LEVELS  300
    int elements = nshell*(nshell+1)/2;
    int swap, pivind, beg[MAX_LEVELS], end[MAX_LEVELS], i=0, L, R; 
    double  piv;

    beg[0]=0; end[0]=elements;
    while (i>=0) {
        L=beg[i]; R=end[i]-1;
        if (L<R) {
            piv=norm_[L];
            pivind=sieve_ind_[L];
            while (L<R) {
                while (norm_[R]>=piv && L<R) R--; 
                if (L<R) { 
                    norm_[L]=norm_[R]; 
                    sieve_ind_[L]=sieve_ind_[R];
                    L++;
                }
                while (norm_[L]<=piv && L<R) L++; 
                if (L<R) {
                    norm_[R]=norm_[L];
                    sieve_ind_[R]=sieve_ind_[L]; 
                    R--;
                }
            }
            norm_[L]=piv; 
            sieve_ind_[L] = pivind; 
            beg[i+1]=L+1; 
            end[i+1]=end[i]; 
            end[i++]=L;
            if (end[i]-beg[i]>end[i-1]-beg[i-1]) {
                swap=beg[i]; beg[i]=beg[i-1]; beg[i-1]=swap;
                swap=end[i]; end[i]=end[i-1]; end[i-1]=swap;
            }
        }
        else {
          i--; 
        }
    }
    //Quicksort finished
#ifdef DEBUG
    fprintf(outfile,"\nSorted Vals:\n"); fflush(outfile);
    for (int i = 0; i<nshell*(nshell+1)/2; i++)
        fprintf(outfile,"\t%d\t%d\t%f\n",i,sieve_ind_[i],norm_[i]);
    fflush(outfile);
#endif
    cut_ind_ = new int[nshell*(nshell+1)/2];
    int crit_ind = nshell*(nshell+1)/2-1;
    mind_ = 0;
    int flag = false;
#ifdef DEBUG
    fprintf(outfile, "\n      Cutoff..."); fflush(outfile);
#endif 

    for (int ij = 0; ij<nshell*(nshell+1)/2; ij++)
    {
        while (crit_ind>=0)
        {
            if (norm_[ij]*norm_[crit_ind]>=SCHWARZ_CUTOFF_*SCHWARZ_CUTOFF_)
                crit_ind--;
            else
                break;
        } 
        if (!flag&&crit_ind<nshell*(nshell+1)/2-1)
        {
            mind_ = ij;
            flag = true;
        }
        cut_ind_[ij] = crit_ind;                
    }
    fprintf(outfile,"\n  %d of %d shell pairings eliminated using schwarz cutoff of %f",mind_,nshell*(nshell+1)/2,SCHWARZ_CUTOFF_); 
#ifdef DEBUG
    fprintf(outfile,"\nCritical indicies:\n"); fflush(outfile);
    for (int i = 0; i<nshell*(nshell+1)/2; i++)
        fprintf(outfile,"\t%d\t%d\n",i,cut_ind_[i]);
    fflush(outfile);
#endif
    //SIEVE READY
}

}}
