#include <tuple>
#include <iostream>
#include <fstream>
#include <vector>
#include <sys/time.h>
#include <cassert>
#include "immintrin.h"
#include <bitset>
using namespace std;

void print_vec(__m512i v) {
  for(int i=0;i<16;i++) {
    cout << *((int *)&v+i) << " ";
  }
  cout << endl;

}

void print_vec(__m512 v) {
  for(int i=0;i<16;i++) {
    cout << *((float *)&v+i) << " ";
  }
  cout << endl;

}

int main(int argc, char *argv[])
{
  int nnodes, nedges;
  ifstream fin(argv[1]); 
  fin >> nnodes >> nedges;

  int *comp = (int *)_mm_malloc(sizeof(int)*nnodes, 64);
  int *comp_new = (int *)_mm_malloc(sizeof(int)*nnodes, 64);
  int *n1 = (int *)_mm_malloc(sizeof(int)*nedges, 64);
  int *n2 = (int *)_mm_malloc(sizeof(int)*nedges, 64);
  float *weight = (float *)_mm_malloc(sizeof(float)*nedges, 64);

  // adjancency lists 
  vector<vector<int> > adjs(nnodes);

  for(int i=0;i<nedges;i++) {
    while(fin >> n1[i] >> n2[i] >> weight[i])i++;
  }

  // tiling
#define TILESIZE 32768

  int dtiles = nnodes / TILESIZE;
  if(nnodes % TILESIZE)dtiles++;
  int ntiles = dtiles * dtiles;
  vector<vector<tuple<int, int, float> > > tiles(ntiles);
  for(int i=0;i<nedges;i++) {
    int tx = n1[i] / TILESIZE;
    int ty = n2[i] / TILESIZE;
    tiles[tx*dtiles+ty].push_back(make_tuple(n1[i], n2[i], weight[i]));
  }

  cout << "nedges: " << nedges << endl;

  nedges = 0;
  vector<vector<vector<tuple<int, int, float>> > > tiles_of_groups(ntiles);
  for(int i=0;i<ntiles;i++) {
    vector<vector<tuple<int, int, float> > > groups;
    for(auto& edge : tiles[i]) {
      bool inserted_flag = false;
      for(auto& g : groups) {
        if(g.size() < 16) {
          bool in_flag = false;
          for(auto& e : g) {
            if(get<1>(e) == get<1>(edge)) {
              in_flag = true;
              break;
            }
          }
          if(!in_flag) {
            g.push_back(edge);
            inserted_flag = true;
            break;
          }
        }
      }
      if(!inserted_flag) {
        vector<tuple<int, int, float> > ng;
        ng.push_back(edge);
        groups.push_back(ng);
      }
    }
    tiles_of_groups.push_back(groups);
    for(auto& g : groups) {
      nedges += g.size();
      while(nedges % 16) {
        nedges++;
      }
    }
  }

  cout << "reordered nedges: " << nedges << endl;

  _mm_free(n1);
  _mm_free(n2);
  _mm_free(weight);
  n1 = (int *)_mm_malloc(sizeof(int)*nedges, 64);
  n2 = (int *)_mm_malloc(sizeof(int)*nedges, 64);
  weight = (float *)_mm_malloc(sizeof(float)*nedges, 64);
  int *active_edges = (int *)_mm_malloc(sizeof(int)*nedges, 64);

  int t = 0;
  for(auto& groups : tiles_of_groups) {
    for(auto& g : groups) {
      for(auto& e : g) {
        n1[t] = get<0>(e);
        n2[t] = get<1>(e);
        weight[t] = get<2>(e);
        t++;
      }
      while(t%16) {
        n1[t] = -1;
        t++;
      }
    }
  }

  assert(t==nedges);


  // initial comptances are inf
  for(int i=0;i<nnodes;i++) {
    comp[i] = i;
    comp_new[i] = i;
  }
  // initially all edges are active
  for(int i=0;i<nedges;i++) {
    active_edges[i] = 1;
  }
  // init adj list
  for(int i=0;i<nedges;i++) {
    if(n1[i] != -1)
      adjs[n1[i]].push_back(i);
  }


  int *wake_list = (int *)_mm_malloc(sizeof(int)*nedges, 64);
  int *wake_list_new = (int *)_mm_malloc(sizeof(int)*nedges, 64);
  int *waked_nodes = (int *)_mm_malloc(sizeof(int)*nnodes, 64);
 int wake_size = nnodes;
  for(int i=0;i<nnodes;i++) {
      wake_list[i] = i;
  }

  int nsteps = 0;

  __m512i vzero = _mm512_set1_epi32(0);
  __m512i vnone = _mm512_set1_epi32(-1);
  __m512 vnoneps = _mm512_set1_ps(-1);

  __m512i sindex[65536];
  int nbits[65536];
  for(int i=0;i<65536;i++) {
    int c = 0;
    int t = i;
    int j = 0;
  
    __declspec(align(64)) int temp[16];
    for(int k=0;k<16;k++) temp[k] = 0;
    while(t)
    {
      if(t & 1) {
        temp[j] = c++;
      };
      j++;
      t /= 2;
    }
    nbits[i] = c;
    sindex[i] = _mm512_load_epi32(temp);
  }
  long time = 0;



  struct timeval tv1, tv2;
  struct timezone tz1, tz2;
  // Bellman-Ford
  while(wake_size != 0) {
	  nsteps++;
    int ws = 0;

	  for(int i=0;i<nnodes;i++) {
		  waked_nodes[i] = 0;
	  }

	  for(int i=0;i<wake_size;i++) {
		  if(!waked_nodes[wake_list[i]]) {
			  waked_nodes[wake_list[i]] = 1;
			  for(int& aj : adjs[wake_list[i]]) {
				  n1[aj] = wake_list[i];
			  }
		  }
	  }


    __m512i vnx_next = _mm512_load_epi32(n1);
    __m512i vny_next = _mm512_load_epi32(n2);

    gettimeofday(&tv1, &tz1);

	  for(int i=0; i<nedges; i+=16) {
		  __m512i vnx = vnx_next;
		  __m512i vny = vny_next;
		  __mmask16 mactive = _mm512_cmpneq_epi32_mask(vnx, vnone);
		  _mm512_store_epi32(n1+i, vnone);

      vnx_next = _mm512_load_epi32(n1+i+16);
      vny_next = _mm512_load_epi32(n2+i+16);

      _mm512_prefetch_i32gather_ps(vnx_next, comp, 4, _MM_HINT_T0);
      _mm512_prefetch_i32gather_ps(vny_next, comp, 4, _MM_HINT_T0);

		  __m512i vdx = _mm512_mask_i32gather_epi32(vnone, mactive, vnx, comp, 4);
		  __m512i vdy = _mm512_mask_i32gather_epi32(vnone, mactive, vny, comp_new, 4);


		  __mmask16 mupdate = _mm512_mask_cmpgt_epi32_mask(mactive, vdy, vdx);
		  _mm512_mask_i32scatter_epi32(comp_new, mupdate, vny, vdx, 4);

		  _mm512_mask_i32scatter_epi32(wake_list_new, mupdate, _mm512_add_epi32(_mm512_set1_epi32(ws), sindex[mupdate]), vny, 4);
		  ws += nbits[mupdate];
	  }

  gettimeofday(&tv2, &tz2);
  time += tv2.tv_usec - tv1.tv_usec + 1000000 * (tv2.tv_sec - tv1.tv_sec);

  for(int i=0;i<ws;i++) {
      comp[wake_list_new[i]] = comp_new[wake_list_new[i]];
    wake_list[i] = wake_list_new[i];
  }
  wake_size = ws;


  }



  cout << "time used (ms): " << time << endl;
  cout << "nsteps: " << nsteps << endl;
  cout << "comp[5]: " << comp[5] << endl;

  return 0;
}
