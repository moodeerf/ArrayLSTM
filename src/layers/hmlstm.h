/*
 *
 * Author: Kamil Rocki
 *
 * Implementation of Stack-LSTM with multiplicative outputs
 * Technical Report, Figure 7.4b
 */

#ifndef _hmLSTM_H_
#define _hmLSTM_H_

#include <vector>
#include <parameters.h>
#include <timelayer.h>
#include <state.h>

#include <containers/cu_matrix.h>

/* gates:

[  0:    N-1 ] = i
[  N:   2N-1 ] = o
[ 2N:   3N-1 ] = f
[ 3N:   4N-1 ] = u

*/

#define N this->N
#define B this->B
#define S this->S

template <typename T>
class hmLSTM : public Timelayer<T> {

	public:
	
		using Timelayer<T>::s;
		
		size_t L = 1; // memory cells per hidden neuron
		
		/* main constructor */
		hmLSTM ( size_t _M, size_t _N, size_t _B,
				 size_t _S, size_t _L = 1 ) : L ( _L ),
				 
			Timelayer<T> ( _M, _N, _B, _S,
			
		{	"hmlstm"		},
		
		{
			/* define states */
			std::make_tuple ( "h", _B, _N ),
			std::make_tuple ( "c", _B, _N * _L ),
			std::make_tuple ( "g", _B, 4 * _N * _L ),
			std::make_tuple ( "g2", _B, 4 * _N * _L )
			
		}, {
		
			/* define params */
			std::make_tuple ( "W", _M, 4 * _N * _L ),
			std::make_tuple ( "U", _N, 4 * _N * _L ),
			std::make_tuple ( "b", 1, 4 * _N * _L ),
			
			std::make_tuple ( "B_ones", _B, 1 )
			
		} ) {
		
			std::cout << "hmlstm levels :" << _L << std::endl;
			
			/*init*/
			matrix_init ( p ( W ) );
			matrix_init ( p ( U ) );
			
			// set biases of f gates to 1
			// (http://jmlr.org/proceedings/papers/v37/jozefowicz15.pdf)
			
			p ( b ).block_forall ( 2 * N * L, N * L, [ = ] () { return 1; } );
			
			//p ( b ).block_forall ( 2 * N * L, N, [ = ] () { return 1; } );
			p ( B_ones ).forall ( [ = ] () { return 1; } );
			
			p ( B_ones ).sync_device();
			
		}
		
		~hmLSTM() { }
		
#define c_gates 3 * N * L * s(t, g).rows()
#define i_gates 0 * N * L * s(t, g).rows()
#define f_gates 2 * N * L * s(t, g).rows()
#define o_gates 1 * N * L * s(t, g).rows()
		
		virtual void forward ( bool apply_dropout, size_t t = 1 ) {
		
			s ( t, x ).sync_device();
			s ( t - 1, h ).sync_device();
			s ( t - 1, c ).sync_device();
			
			// put in 2 separate streams
			cublasSetStream ( handle, streams[1] );
			CU_GEMM ( s ( t, g ), s ( t, x ), p ( W ), false, false, 1, 0 );
			
			cublasSetStream ( handle, streams[2] );
			CU_GEMM ( s ( t, g2 ), s ( t - 1, h ), p ( U ), false, false, 1, 0 );
			
			sync_stream ( 1 );
			sync_stream ( 2 );
			
			//fused
			cu_elementwise_hmlstm_forward (
				& ( s ( t, g ).cu_data[0] ),
				& ( s ( t, g2 ).cu_data[0] ),
				& ( p ( b ).cu_data[0] ),
				& ( s ( t, h ).cu_data[0] ),
				& ( s ( t, c ).cu_data[0] ),
				& ( s ( t - 1, c ).cu_data[0] ),
				N, L, s ( t, c ).rows() );
				
			s ( t, c ).sync_host();
			s ( t, h ).sync_host();
			
		}
		
		virtual void backward ( bool apply_dropout, size_t t ) {
		
			// error coming from higher layers: dh[t] = dy[t]
			
			cu_elementwise_hmlstm_backward (
				& ( g ( t, g ).cu_data[0] ),
				& ( g ( t, y ).cu_data[0] ),
				& ( s ( t, c ).cu_data[0] ),
				& ( g ( t, c ).cu_data[0] ),
				& ( s ( t, g ).cu_data[0] ),
				& ( s ( t - 1, c ).cu_data[0] ),
				& ( g ( t - 1, c ).cu_data[0] ),
				s ( t, h ).cu_data,
				N, L, g ( t, c ).rows() );
				
			//backprop through linear part (forward pass step 1)
			//these are computed in parallel
			cublasSetStream ( handle, streams[1] );
			CU_GEMM ( d ( b ), p ( B_ones ), g ( t, g ), true, false );
			
			cublasSetStream ( handle, streams[2] );
			CU_GEMM ( d ( U ), s ( t - 1, h ), g ( t, g ), true, false );
			
			cublasSetStream ( handle, streams[3] );
			CU_GEMM ( d ( W ), s ( t, x ), g ( t, g ), true, false );
			
			cublasSetStream ( handle, streams[4] );
			//backprop into inputs for lower layers
			CU_GEMM ( g ( t, x ), g ( t, g ), p ( W ), false, true );
			
			cublasSetStream ( handle, streams[5] );
			//carry - h state
			CU_GEMM ( g ( t - 1, y ), g ( t, g ), p ( U ), false , true );
			
			sync_stream ( 1 );
			sync_stream ( 2 );
			sync_stream ( 3 );
			sync_stream ( 4 );
			sync_stream ( 5 );
			
			if ( t == 1 )
				cu_elementwise_sparselstm_sparsity ( N, L, g ( t, c ).rows(), S, & ( p ( b ).cu_data[0] ), 0.005f );
				
				
		}
		
		virtual void reset ( dtype std ) {
		
			randn ( s ( 0, h ), ( dtype ) 0, ( dtype ) std );
			randn ( s ( 0, c ), ( dtype ) 0, ( dtype ) std );
			// s ( 0, h ).cu_zero();
			// s ( 0, c ).cu_zero();
		}
		
		/* optional */
		/* copy constr */
		//hmLSTM( const hmLSTM& src) : Timelayer(src.M, src.N, src.B, src.S)  { }
		
		/* assignent operator */
		//hmLSTM& operator=( const hmLSTM& src) { Timelayer::operator=(src); return *this; }
		
};

#undef N
#undef B
#undef S
#undef i_gates
#undef o_gates
#undef c_gates
#undef f_gates

#endif /* _hmLSTM_H_ */
