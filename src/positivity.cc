#include <base/quadrature_lib.h>

#include <fe/fe_values.h>
#include <fe/fe_values_extractors.h>

#include <dofs/dof_handler.h>

#include "equation.h"
#include "claw.h"

using namespace dealii;

//-----------------------------------------------------------------------------
// Positivity limiter of Zhang-Shu
//-----------------------------------------------------------------------------
template <int dim>
void ConservationLaw<dim>::apply_positivity_limiter ()
{
   if(fe.degree == 0) return;
   
   const double gas_gamma = EulerEquations<dim>::gas_gamma;
   unsigned int density_component = EulerEquations<dim>::density_component;
   unsigned int energy_component = EulerEquations<dim>::energy_component;
   
   // Find mininimum density and pressure in the whole grid
   double eps = 1.0e-13;
   {
      std::vector<unsigned int> cell_indices (fe0.dofs_per_cell);
      
      typename DoFHandler<dim>::active_cell_iterator
         cell0 = dof_handler0.begin_active(),
         endc0 = dof_handler0.end();
      
      for (; cell0 != endc0; ++cell0)
      {
         cell0->get_dof_indices (cell_indices);
         std::vector<double> W(EulerEquations<dim>::n_components);
         for(unsigned int i=0; i<EulerEquations<dim>::n_components; ++i)
            W[i] = cell_average(cell_indices[i]);
         eps = std::min(eps, W[density_component]);
         double pressure = EulerEquations<dim>::template compute_pressure<double> (W);
         eps = std::min(eps, pressure);
      }
      
      if(eps < 0.0)
      {
         std::cout << "Fatal: Negative states\n";
         exit(0);
      }
   }
   
   // Need 2N - 3 >= degree for the quadrature to be exact.
   unsigned int N = (fe.degree + 3)/2;
   if((fe.degree+3)%2 != 0) N += 1;
   QGaussLobatto<dim>  quadrature_formula(N);
   const unsigned int n_q_points = quadrature_formula.size();
   FEValues<dim> fe_values (fe, quadrature_formula, update_values);
   std::vector<double> density_values(n_q_points), energy_values(n_q_points);
   std::vector< Tensor<1,dim> > momentum_values(n_q_points);
   std::vector<unsigned int> local_dof_indices (fe.dofs_per_cell);
   std::vector<unsigned int> cell_indices (fe0.dofs_per_cell);
   
   const FEValuesExtractors::Scalar density  (dim);
   const FEValuesExtractors::Scalar energy   (dim+1);
   const FEValuesExtractors::Vector momentum (0);
   
   typename DoFHandler<dim>::active_cell_iterator
      cell = dof_handler.begin_active(),
      endc = dof_handler.end(),
      cell0 = dof_handler0.begin_active(),
      endc0 = dof_handler0.end();
   
   for(; cell != endc; ++cell, ++cell0)
   {
      fe_values.reinit(cell);
      cell->get_dof_indices (local_dof_indices);
      cell0->get_dof_indices (cell_indices);
      
      // First limit density
      fe_values[density].get_function_values(current_solution, density_values);
      
      // find minimum density at GLL points
      double rho_min = 1.0e20;
      for(unsigned int q=0; q<n_q_points; ++q)
         rho_min = std::min(rho_min, density_values[q]);
      
      double density_average = cell_average(cell_indices[density_component]);
      double rat = std::fabs(density_average - eps) /
                   (std::fabs(density_average - rho_min) + 1.0e-13);
      double theta1 = std::min(rat, 1.0);
      
      for(unsigned int i=0; i<fe.dofs_per_cell; ++i)
      {
         unsigned int comp_i = fe.system_to_component_index(i).first;
         if(comp_i == density_component)
            current_solution(local_dof_indices[i]) =
                 theta1         * current_solution(local_dof_indices[i])
               + (1.0 - theta1) * density_average;
      }
      
      // now limit pressure
      fe_values[density].get_function_values(current_solution, density_values);
      fe_values[momentum].get_function_values(current_solution, momentum_values);
      fe_values[energy].get_function_values(current_solution, energy_values);
      
      double energy_average = cell_average(cell_indices[energy_component]);
      Tensor<1,dim> momentum_average;
      for(unsigned int i=0; i<dim; ++i)
         momentum_average[i] = cell_average(cell_indices[i]);
      
      double theta2 = 1.0;
      for(unsigned int q=0; q<n_q_points; ++q)
      {
         double pressure = (gas_gamma-1.0)*(energy_values[q] -
                                            0.5*momentum_values[q].norm_square()/density_values[q]);
         if(pressure < eps)
         {
            double drho = density_values[q] - density_average;
            Tensor<1,dim> dm = momentum_values[q] - momentum_average;
            double dE = energy_values[q] - energy_average;
            double a1 = 2.0*drho*dE - dm*dm;
            double b1 = 2.0*drho*(energy_average - eps/(gas_gamma-1.0))
                      + 2.0*density_average*dE
                      - 2.0*momentum_average*dm;
            double c1 = 2.0*density_average*energy_average
                      - momentum_average*momentum_average
                      - 2.0*eps*density_average/(gas_gamma-1.0);
            // Divide by a1 to avoid round-off error
            b1 /= a1; c1 /= a1;
            double D = std::sqrt( std::fabs(b1*b1 - 4.0*c1) );
            double t1 = 0.5*(-b1 - D);
            double t2 = 0.5*(-b1 + D);
            double t;
            if(t1 > -1.0e-12 && t1 < 1.0 + 1.0e-12)
               t = t1;
            else if(t2 > -1.0e-12 && t2 < 1.0 + 1.0e-12)
               t = t2;
            else
            {
               std::cout << "Problem in positivity limiter\n";
               std::cout << "\t a1, b1, c1 = " << a1 << " " << b1 << " " << c1 << "\n";
               std::cout << "\t t1, t2 = " << t1 << " " << t2 << "\n";
               std::cout << "\t eps, rho_min = " << eps << " " << rho_min << "\n";
               std::cout << "\t theta1 = " << theta1 << "\n";
               std::cout << "\t pressure = " << pressure << "\n";
               exit(0);
            }
            // t should strictly lie in [0,1]
            t = std::min(1.0, t);
            t = std::max(0.0, t);
            // Need t < 1.0. If t==1 upto machine precision
            // then we are suffering from round off error.
            // In this case we take the cell average value, t=0.
            if(std::fabs(1.0-t) < 1.0e-14) t = 0.0;
            theta2 = std::min(theta2, t);
         }
      }
      
      for(unsigned int i=0; i<fe.dofs_per_cell; ++i)
      {
         unsigned int comp_i = fe.system_to_component_index(i).first;
         current_solution(local_dof_indices[i]) =
               theta2         * current_solution(local_dof_indices[i])
            + (1.0 - theta2)  * cell_average(cell_indices[comp_i]);
      }
      
   }
}

template class ConservationLaw<2>;