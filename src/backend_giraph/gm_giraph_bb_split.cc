#include <stdio.h>
#include "gm_backend_giraph.h"
#include "gm_error.h"
#include "gm_code_writer.h"
#include "gm_frontend.h"
#include "gm_transform_helper.h"
#include "gm_traverse.h"
#include <set>

typedef gm_giraph_basic_block giraph_bb;

//------------------------------------------------------------------
// Split Basic Blocks
//   [Prepare Step]
//     - Find BB that contains communication, and add into a list
//     - Mark communication foreach statement (assign ID)
//------------------------------------------------------------------
class giraph_find_comm_vertex_bb: public giraph_apply_bb_ast
{
public:

    giraph_find_comm_vertex_bb(gm_giraph_beinfo* g) 
    {
        gen = g;
        set_for_sent(true);
        current_outer_loop = NULL;
    }

    virtual bool apply(ast_sent* s) 
    {
        // receiver should be empty.
        assert(is_under_receiver_traverse() == false);

        giraph_bb* curr = get_curr_BB();

        // only look at vertex BB
        if (!curr->is_vertex()) 
            return true;

        // neighborhood looking foreach statement is a communicating bb
        if (s->get_nodetype() == AST_FOREACH)
        {
            ast_foreach* fe = (ast_foreach*) s;

            if (gm_is_all_graph_iter_type(fe->get_iter_type())) {
                current_outer_loop = fe;
                fe->add_info_bool(GIRAPH_FLAG_IS_OUTER_LOOP, true);
                return true;
            }

            //curr->set_has_sender(true);
   
            gen->add_communication_unit_nested(fe); // adding inner loop
            fe->add_info_bool(GIRAPH_FLAG_IS_INNER_LOOP, true);

            // add the foreach loop as 'receiver' of this state, temporariliy.
            // (Receiver loop will be moved away later)
            curr->add_nested_receiver(fe);

            // list of bbs that should be splited
            target_bb.insert(curr);

            // mark current outer loop to have communication
            assert(current_outer_loop != NULL);
            current_outer_loop->add_info_bool(GIRAPH_FLAG_HAS_COMMUNICATION, true);
        }
        else if (s->get_nodetype() == AST_ASSIGN)
        {
            if (s->find_info_ptr(GIRAPH_FLAG_SENT_BLOCK_FOR_RANDOM_WRITE_ASSIGN) != NULL)
            {
                ast_assign* a = (ast_assign*) s;
                ast_sentblock* sb = (ast_sentblock*) (s->find_info_ptr(GIRAPH_FLAG_SENT_BLOCK_FOR_RANDOM_WRITE_ASSIGN)); 
                ast_field* f = a->get_lhs_field();
                gm_symtab_entry* sym = f->get_first()->getSymInfo();
                
                gen->add_communication_unit_random_write(sb, sym);
                gen->add_random_write_sent(sb, sym, s);
                curr->add_random_write_receiver(sb, sym);

                target_bb.insert(curr);

                assert(current_outer_loop != NULL);
                current_outer_loop->add_info_bool(GIRAPH_FLAG_HAS_COMMUNICATION_RANDOM, true);
            }
        }

        return true;
    }

    std::set<giraph_bb*>& get_target_basic_blocks()  {return target_bb;}

private:
    gm_giraph_beinfo* gen;
    std::set<giraph_bb*> target_bb;
    ast_foreach* current_outer_loop;

};


static giraph_bb* split_vertex_BB(giraph_bb* BB, gm_giraph_beinfo* gen);

void gm_giraph_opt_split_comm_ebb::process(ast_procdef* p)
{
    gm_giraph_beinfo* info = (gm_giraph_beinfo*) FE.get_backend_info(p);
    giraph_bb* entry = info->get_entry_basic_block();

    //-------------------------------------------
    // find Basic Blocks that contain nested communication
    //-------------------------------------------
    giraph_find_comm_vertex_bb T(info);
    giraph_bb_apply_only_once(entry, &T);

    std::set<giraph_bb*>& BB_list = T.get_target_basic_blocks() ;

    //-------------------------------------------
    // split BB into two
    ///  BB => 
    //   BB1 (send) -> seq -> BB2 (receive) 
    //-------------------------------------------
    std::set<giraph_bb*>::iterator I;
    for(I=BB_list.begin(); I!= BB_list.end(); I++) {
        giraph_bb* BB = *I;
        giraph_bb* BB2 = split_vertex_BB(BB, info);

    }
}

//    [prev -> BB -> next] ==>
//    [prev -> BB(S) -> new_seq -> BB(R) -> next]
giraph_bb* split_vertex_BB(giraph_bb* BB, gm_giraph_beinfo* gen)
{
    //printf("splitting BB id = %d\n", BB->get_id());

    assert(BB->is_vertex());
    //assert(BB->has_sender());
    assert(BB->has_receiver());
    assert(BB->get_num_entries() == 1);
    assert(BB->get_num_exits() == 1);

    giraph_bb* prev = BB->get_nth_entry(0);
    giraph_bb* next = BB->get_nth_exit(0);

    assert(!prev->is_vertex());
    assert(!next->is_vertex());
    assert(next->get_num_entries() == 1);

    giraph_bb* new_seq = new giraph_bb(gen->issue_basicblock_id());
    new_seq->set_after_vertex(true);

    giraph_bb* new_BB = new giraph_bb(gen->issue_basicblock_id(), GM_GIRAPH_BBTYPE_BEGIN_VERTEX);

    //--------------------------------------
    // migrate receiver list to new_BB
    //--------------------------------------
    std::list<gm_giraph_comm_unit>& L = BB->get_receivers(); 
    std::list<gm_giraph_comm_unit>::iterator I;
    for(I=L.begin(); I!= L.end(); I++)
    {
        new_BB ->add_receiver(*I);
    }
    BB->clear_receivers();

    // insert basic blocks
    BB->remove_all_exits();
    next->remove_all_entries();

    BB->add_exit(new_seq);
    new_seq->add_exit(new_BB);
    new_BB->add_exit(next);

    std::list<gm_giraph_basic_block*>& BBLIST = gen->get_basic_blocks();
    BBLIST.push_back(new_seq);
    BBLIST.push_back(new_BB);

    return new_BB;

}


