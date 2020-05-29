/*
 * Parser.cpp
 *
 *  Created on: May 9, 2020
 *      Author: teng
 */

#include "../geometry/MyPolygon.h"
#include <fstream>
#include "../index/RTree.h"
#include <queue>
#include <boost/program_options.hpp>

namespace po = boost::program_options;
using namespace std;

#define MAX_THREAD_NUM 100

// some shared parameters
pthread_mutex_t poly_lock;
bool stop = false;


class queue_element{
public:
	int id = 0;
	vector<MyPolygon *> polys;
	queue_element(int i){
		id = i;
	}
	~queue_element(){
		for(MyPolygon *p:polys){
			delete p;
		}
		polys.clear();
	}
};
queue<queue_element *> shared_queue;

RTree<MyPolygon *, double, 2, double> tree;

class query_context{
public:
	int thread_id = 0;
	MyPolygon *target = NULL;
	int max_dimx = 40;
	int max_dimy = 40;
	bool use_partition = false;
	int found = 0;
	query_context(){}
};

bool MySearchCallback(MyPolygon *poly, void* arg){
	query_context *ctx = (query_context *)arg;
	if(ctx->use_partition&&!poly->ispartitioned()){
		poly->partition(ctx->max_dimx, ctx->max_dimy);
	}
	ctx->found += poly->contain(ctx->target, ctx->use_partition);
	// keep going until all hit objects are found
	return true;
}

void *query(void *args){
	query_context *ctx = (query_context *)args;
	pthread_mutex_lock(&poly_lock);
	log("thread %d is started",ctx->thread_id);
	pthread_mutex_unlock(&poly_lock);

	while(!stop||shared_queue.size()>0){
		queue_element *elem = NULL;
		pthread_mutex_lock(&poly_lock);
		if(!shared_queue.empty()){
			elem = shared_queue.front();
			shared_queue.pop();
		}
		pthread_mutex_unlock(&poly_lock);
		// queue is empty but not stopped
		if(!elem){
			usleep(20);
			continue;
		}
		for(MyPolygon *poly:elem->polys){
			ctx->target = poly;
			Pixel *px = poly->getMBB();
			tree.Search(px->low, px->high, MySearchCallback, (void *)ctx);
		}
		delete elem;
	}

	return NULL;
}

int main(int argc, char** argv) {
	string source_path;
	string target_path;
	int num_threads = get_num_threads();
	bool use_partition = false;
	int max_dimx = 40;
	int max_dimy = 40;
	po::options_description desc("query usage");
	desc.add_options()
		("help,h", "produce help message")
		("partition,p", "query with inner partition")
		("source,s", po::value<string>(&source_path), "path to the source")
		("target,t", po::value<string>(&target_path), "path to the target")
		("threads,n", po::value<int>(&num_threads), "number of threads")
		("max_dimx,x", po::value<int>(&max_dimx), "max dimension on horizontal")
		("max_dimy,y", po::value<int>(&max_dimy), "max dimension on vertical")
		;
	po::variables_map vm;
	po::store(po::parse_command_line(argc, argv, desc), vm);
	if (vm.count("help")) {
		cout << desc << "\n";
		return 0;
	}
	po::notify(vm);
	if(vm.count("partition")){
		use_partition = true;
	}
	if(!vm.count("source")||!vm.count("target")){
		cout << desc << "\n";
		return 0;
	}

	timeval start = get_cur_time();

	vector<MyPolygon *> source = MyPolygon::load_binary_file(source_path.c_str());
	logt("loaded %ld polygons", start, source.size());
	for(MyPolygon *p:source){
		tree.Insert(p->getMBB()->low, p->getMBB()->high, p);
	}
	logt("building R-Tree", start);
	pthread_t threads[num_threads];
	query_context ctx[num_threads];
	for(int i=0;i<num_threads;i++){
		ctx[i].thread_id = i;
		ctx[i].max_dimx = max_dimx;
		ctx[i].max_dimy = max_dimy;
		ctx[i].use_partition = use_partition;
	}
	for(int i=0;i<num_threads;i++){
		pthread_create(&threads[i], NULL, query, (void *)&ctx[i]);
	}

	ifstream is;
	is.open(target_path.c_str(), ios::in | ios::binary);
	int eid = 0;
	queue_element *elem = new queue_element(eid++);
	int loaded = 0;
	while(!is.eof()){
		MyPolygon *poly = MyPolygon::read_polygon_binary_file(is);
		if(!poly){
			continue;
		}
		elem->polys.push_back(poly);
		if(elem->polys.size()==20){
			while(shared_queue.size()>2*num_threads){
				usleep(20);
			}
			pthread_mutex_lock(&poly_lock);
			shared_queue.push(elem);
			pthread_mutex_unlock(&poly_lock);
			elem = new queue_element(eid++);
		}
		if(++loaded%100000==0){
			log("queried %d polygons",loaded);
		}
	}
	if(elem->polys.size()>0){
		pthread_mutex_lock(&poly_lock);
		shared_queue.push(elem);
		pthread_mutex_unlock(&poly_lock);
	}
	stop = true;

	for(int i = 0; i < num_threads; i++ ){
		void *status;
		pthread_join(threads[i], &status);
	}
	logt("queried %d polygons",start,loaded);

	for(MyPolygon *p:source){
		delete p;
	}
	return 0;
}


