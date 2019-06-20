//
// Copyright 2019 Ettus Research, a National Instruments Brand
//
// SPDX-License-Identifier: GPL-3.0-or-later
//

#ifndef INCLUDED_LIBUHD_GRAPH_HPP
#define INCLUDED_LIBUHD_GRAPH_HPP

#include <uhd/rfnoc/graph_edge.hpp>
#include <uhd/rfnoc/actions.hpp>
#include <uhd/rfnoc/node.hpp>
#include <boost/graph/adjacency_list.hpp>
#include <tuple>
#include <memory>
#include <deque>
#include <atomic>

namespace uhd { namespace rfnoc { namespace detail {

//! Container for the logical graph within an uhd::rfnoc_graph
class graph_t
{
public:
    using uptr = std::unique_ptr<graph_t>;
    //! A shorthand for a pointer to a node
    using node_ref_t = uhd::rfnoc::node_t*;
    //! Shorthand to existing graph_edge_t
    using graph_edge_t = uhd::rfnoc::graph_edge_t;

    /*! Add a connection to the graph
     *
     * After this function returns, the nodes will be considered connected
     * along the ports specified in \p edge_info.
     *
     * \param src_node A reference to the source node
     * \param dst_node A reference to the destination node
     * \param edge_info Information about the type of edge
     */
    void connect(node_ref_t src_node, node_ref_t dst_node, graph_edge_t edge_info);

    //void disconnect(node_ref_t src_node,
        //node_ref_t dst_node,
        //const size_t src_port,
        //const size_t dst_port);
        //

    /*! Commit graph and run initial checks
     *
     * This method can be called anytime, but it's intended to be called when
     * the graph has been committed. It will run checks on the graph and run a
     * property propagation.
     *
     * \throws uhd::resolve_error if the properties fail to resolve.
     */
    void commit();

    /*! Opposite of commit()
     *
     * Calling this will disable property propagation until commit() has been
     * called an equal number of times.
     */
    void release();

    /*! Return a list of all edges
     */
    std::vector<graph_edge_t> enumerate_edges();

private:
    friend class graph_accessor_t;

    /**************************************************************************
     * Graph-related types
     *************************************************************************/
    // Naming conventions:
    // - 'vertex' and 'node' are generally ambiguous in a graph context, but
    //   we'll use vertex for BGL related items, and node for RFNoC nodes
    // - We may use CamelCase occasionally if it fits the BGL examples and/or
    //   reference designs, in case someone needs to learn BGL to understand
    //   this code

    struct vertex_property_t
    {
        enum { num = 4000 };
        typedef boost::vertex_property_tag kind;
    };
    using RfnocVertexProperty = boost::property<vertex_property_t, node_ref_t>;

    struct edge_property_t
    {
        enum { num = 4001 };
        typedef boost::edge_property_tag kind;
    };
    using RfnocEdgeProperty = boost::property<edge_property_t, graph_edge_t>;

    /*! The type of the BGL graph we're using
     *
     * - It is bidirectional because we need to access both in_edges and
     *   out_edges
     * - All container types are according to the BGL manual recommendations for
     *   this kind of graph
     */
    using rfnoc_graph_t = boost::adjacency_list<boost::vecS,
        boost::vecS,
        boost::bidirectionalS,
        RfnocVertexProperty,
        RfnocEdgeProperty>;

    using vertex_list_t = std::list<rfnoc_graph_t::vertex_descriptor>;

    template <bool forward_edges_only = true>
    struct ForwardBackwardEdgePredicate
    {
        ForwardBackwardEdgePredicate() {} // Default ctor is required
        ForwardBackwardEdgePredicate(rfnoc_graph_t& graph) : _graph(&graph) {}

        template <typename Edge>
        bool operator()(const Edge& e) const
        {
            graph_edge_t edge_info = boost::get(edge_property_t(), *_graph, e);
            return edge_info.property_propagation_active == forward_edges_only;
        }

    private:
        // Don't make any attribute const, because default assignment operator
        // is also required
        rfnoc_graph_t* _graph;
    };

    using ForwardEdgePredicate = ForwardBackwardEdgePredicate<true>;
    using BackEdgePredicate = ForwardBackwardEdgePredicate<false>;

    //! Vertex predicate, only selects nodes with dirty props
    struct DirtyNodePredicate;

    //! Vertex predicate, returns specific existing nodes
    struct FindNodePredicate;

    /**************************************************************************
     * Other private types
     *************************************************************************/
    using node_map_t = std::map<node_ref_t, rfnoc_graph_t::vertex_descriptor>;

    /**************************************************************************
     * The Algorithm
     *************************************************************************/
    /*! Implementation of the property propagation algorithm
     */
    void resolve_all_properties();

    /**************************************************************************
     * Action API
     *************************************************************************/
    /*! Entrypoint for action delivery
     *
     * When a node invokes its node_t::post_action() function, eventually that
     * call lands here. This function acts as a mailman, that is, it figures out
     * which edge on which node is supposed to receive this action, and delivers
     * it via the node_t::receive_action() method.
     * Note since this is private, nodes can't directly access this functions.
     * We provide a lambda to nodes for that purpose.
     *
     * When an action is posted, that may trigger further actions. In order not
     * to go into infinite recursion, this function is also responsible for
     * serializing the actions. Even so, it is possible that, due to
     * misconfiguration of nodes and their behaviour, a cascade of actions is
     * posted that never stops. Therefore, another responsibility of this
     * function is to track the number of follow-up messages sent, and terminate
     * an infinite cycle of messages.
     *
     * \param src_node Reference to the node where the post_action() call is
     *                 originating from
     * \param src_edge The edge on that node where the action is being posted to.
     *                 Note that its the edge from the node's point of view, so
     *                 if src_edge.type == OUTPUT_EDGE, then the node posted to
     *                 its output edge.
     *
     * \throws uhd::runtime_error if it has to terminate a infinite cascade of
     *         actions
     */
    void enqueue_action(
        node_ref_t src_node, res_source_info src_edge, action_info::sptr action);

    /**************************************************************************
     * Private graph helpers
     *************************************************************************/
    template <typename VertexContainerType>
    std::vector<node_ref_t> _vertices_to_nodes(VertexContainerType&& vertex_container)
    {
        std::vector<node_ref_t> result{};
        result.reserve(vertex_container.size());
        for (const auto& vertex_descriptor : vertex_container) {
            result.push_back(boost::get(vertex_property_t(), _graph, vertex_descriptor));
        }
        return result;
    }

    /*! Returns a list of all nodes that have dirty properties.
     */
    vertex_list_t _find_dirty_nodes();

    /*! Returns nodes in topologically sorted order
     *
     *
     * \throws uhd::runtime_error if the graph was not sortable
     */
    vertex_list_t _get_topo_sorted_nodes();

    /*! Add a node, but only if it's not already in the graph.
     *
     * If it's already there, do nothing.
     */
    void _add_node(node_ref_t node);

    /*! Find the neighbouring node for \p origin based on \p port_info
     *
     * This function will check port_info to identify the port number and the
     * direction (input or output) from \p origin. It will then return a
     * reference to the node that is attached to the node \p origin if such a
     * node exists, and the edge info.
     *
     * If port_info.type == res_source_info::INPUT_EDGE, then port_info.instance
     * will equal the return value's dst_port value.
     *
     * \returns A valid reference to the neighbouring node, or nullptr if no
     *          such node exists, and the corresponding edge info.
     */
    std::pair<node_ref_t, graph_edge_t> _find_neighbour(
        rfnoc_graph_t::vertex_descriptor origin, res_source_info port_info);

    /*! Forward all edge properties from this node (\p origin) to the
     * neighbouring ones
     *
     */
    void _forward_edge_props(rfnoc_graph_t::vertex_descriptor origin);

    /*! Check that the edge properties on both sides of the edge are equal
     *
     * \returns false if edge properties are not consistent
     */
    bool _assert_edge_props_consistent(rfnoc_graph_t::edge_descriptor edge);

    /*! Query all blocks on their topology
     *
     * \throws uhd::runtime_error if any of the blocks doesn't like its
     * configuration
     */
    void _check_topology();

    /**************************************************************************
     * Attributes
     *************************************************************************/
    //! Storage for the actual graph
    rfnoc_graph_t _graph;

    //! Map to do a lookup node_ref_t -> vertex descriptor.
    //
    // This is technically redundant, but helps us check quickly and easily if
    // a node is already in the graph, and to yank out the appropriate node
    // descriptor without having to traverse the graph. The rfnoc_graph_t is not
    // efficient for lookups of vertices.
    node_map_t _node_map;

    using action_tuple_t = std::tuple<node_ref_t, res_source_info, action_info::sptr>;

    //! FIFO for incoming actions
    std::deque<action_tuple_t> _action_queue;

    //! Flag to ensure serialized handling of actions
    std::atomic_flag _action_handling_ongoing;

    //! Mutex for to avoid the user from sending one message before another
    // message is sent
    std::recursive_mutex _action_mutex;

    //! This counter gets decremented everytime commit() is called. When zero,
    // the graph is committed.
    std::atomic<size_t> _release_count{1};
};


}}} /* namespace uhd::rfnoc::detail */

#endif /* INCLUDED_LIBUHD_GRAPH_HPP */
