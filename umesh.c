#include "umesh.h"
#include "params.h"
#include "shared.h"
#include <assert.h>
#include <stdlib.h>

// Converts the lists of cell counts to a list of offsets
void convert_cell_counts_to_offsets(UnstructuredMesh* umesh);

// Fill in the list of cells surrounding nodes
void fill_nodes_to_cells(UnstructuredMesh* umesh);

// Determine the cells that neighbour other cells
void fill_cells_to_cells(UnstructuredMesh* umesh);

// Determine the nodes that surround other nodes
void fill_nodes_to_nodes(UnstructuredMesh* umesh);

// Initialises the unstructured mesh variables
size_t initialise_unstructured_mesh(UnstructuredMesh* umesh) {
  // Allocate the data structures that we now know the sizes of
  size_t allocated = allocate_data(&umesh->cell_centroids_x, umesh->ncells);
  allocated += allocate_data(&umesh->cell_centroids_y, umesh->ncells);
  allocated += allocate_data(&umesh->cell_centroids_z, umesh->ncells);
  allocated += allocate_int_data(&umesh->nodes_offsets, umesh->nnodes + 1);
  allocated += allocate_int_data(&umesh->cells_offsets, umesh->ncells + 1);
  allocated +=
      allocate_int_data(&umesh->nodes_to_nodes_offsets, umesh->nnodes + 1);
  allocated += allocate_int_data(&umesh->cells_to_nodes,
                                 umesh->ncells * umesh->nnodes_by_cell);
  allocated += allocate_int_data(&umesh->nodes_to_nodes,
                                 umesh->nnodes * umesh->nnodes_by_node);
  allocated += allocate_int_data(&umesh->nodes_to_cells,
                                 umesh->ncells * umesh->nnodes_by_cell);
  allocated += allocate_int_data(&umesh->cells_to_cells,
                                 umesh->ncells * umesh->nnodes_by_cell);
  return allocated;
}

// Reads the nodes data from the unstructured mesh definition
size_t read_unstructured_mesh(UnstructuredMesh* umesh, double*** cell_variables,
                              int nvars) {
  // Open the files
  FILE* node_fp = fopen(umesh->node_filename, "r");
  if (!node_fp) {
    TERMINATE("Could not open the parameter file: %s.\n", umesh->node_filename);
  }

  // Fetch the first line of the nodes file
  char buf[MAX_STR_LEN];
  char* line = buf;

  // Read the number of nodes, for allocation
  fgets(line, MAX_STR_LEN, node_fp);
  skip_whitespace(&line);
  sscanf(line, "%d", &umesh->nnodes);

  size_t allocated = allocate_data(&umesh->nodes_x0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_y0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_x1, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_y1, umesh->nnodes);
  allocated += allocate_int_data(&umesh->boundary_index, umesh->nnodes);

  // Loop through the node file, storing all of the nodes in our data structure
  umesh->nboundary_nodes = 0;
  while (fgets(line, MAX_STR_LEN, node_fp)) {
    int index;
    int is_boundary;
    int discard;
    sscanf(line, "%d", &index);
    sscanf(line, "%d%lf%lf%d", &discard, &umesh->nodes_x0[(index)],
           &umesh->nodes_y0[(index)], &is_boundary);

    umesh->boundary_index[(index)] =
        (is_boundary) ? umesh->nboundary_nodes++ : IS_INTERIOR;
  }

  allocated += allocate_data(&umesh->boundary_normal_x, umesh->nboundary_nodes);
  allocated += allocate_data(&umesh->boundary_normal_y, umesh->nboundary_nodes);
  allocated += allocate_int_data(&umesh->boundary_type, umesh->nboundary_nodes);

  fclose(node_fp);

  // Open the files
  FILE* ele_fp = fopen(umesh->ele_filename, "r");
  if (!ele_fp) {
    TERMINATE("Could not open the parameter file: %s.\n", umesh->ele_filename);
  }

  // Read meta data from the element file
  fgets(line, MAX_STR_LEN, ele_fp);
  skip_whitespace(&line);
  sscanf(line, "%d%d%d", &umesh->ncells, &umesh->nnodes_by_cell,
         &umesh->nregional_variables);

  int boundary_edge_index = 0;
  int* boundary_face_list;

  assert(nvars == umesh->nregional_variables &&
         "The number of variables passed to read_element_data \
      doesn't match the input file.");

  allocated += initialise_unstructured_mesh(umesh);
  for (int ii = 0; ii < umesh->nregional_variables; ++ii) {
    allocated += allocate_data(cell_variables[ii], umesh->ncells);
  }

  allocate_int_data(&boundary_face_list, umesh->nboundary_nodes * 2);

  // Loop through the element file and flatten into data structure
  while (fgets(line, MAX_STR_LEN, ele_fp)) {
    // Read in the index
    int index;
    char* line_temp = line;
    read_token(&line_temp, "%d", &index);

    // Read in each of the node locations
    int node[umesh->nnodes_by_cell];
    for (int ii = 0; ii < umesh->nnodes_by_cell; ++ii) {
      read_token(&line_temp, "%d", &node[ii]);
    }

    // Check that we are storing the nodes in the correct order
    double A = 0.0;
    for (int ii = 0; ii < umesh->nnodes_by_cell; ++ii) {
      const int ii2 = (ii + 1) % umesh->nnodes_by_cell;
      A += (umesh->nodes_x0[node[ii]] + umesh->nodes_x0[node[ii2]]) *
           (umesh->nodes_y0[node[ii2]] - umesh->nodes_y0[node[ii]]);
    }
    assert(A > 0.0 && "Nodes are not stored in counter-clockwise order.\n");

    // Read in each of the regional variables
    for (int ii = 0; ii < umesh->nregional_variables; ++ii) {
      read_token(&line_temp, "%lf", &(*(cell_variables[ii]))[index]);
    }

    // Store the cell offsets in case of future mixed cell geometry
    umesh->cells_offsets[(index + 1)] =
        umesh->cells_offsets[(index)] + umesh->nnodes_by_cell;

    // Store cells to nodes and check if we are at a boundary edge cell
    int nboundary_nodes = 0;
    for (int nn = 0; nn < umesh->nnodes_by_cell; ++nn) {
      umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + nn] = node[(nn)];
      umesh->nodes_offsets[(node[(nn)]) + 1]++;
      nboundary_nodes += (umesh->boundary_index[(node[nn])] != IS_INTERIOR);
    }

    // Only store edges that are on the boundary, maintaining the
    // counter-clockwise order
    if (nboundary_nodes == 2) {
      for (int nn = 0; nn < umesh->nnodes_by_cell; ++nn) {
        const int next_node_index =
            (nn + 1 == umesh->nnodes_by_cell ? 0 : nn + 1);
        if (umesh->boundary_index[(node[nn])] != IS_INTERIOR &&
            umesh->boundary_index[(node[next_node_index])] != IS_INTERIOR) {
          boundary_face_list[boundary_edge_index++] = node[nn];
          boundary_face_list[boundary_edge_index++] = node[next_node_index];
          break;
        }
      }
    }
  }

  convert_cell_counts_to_offsets(umesh);
  fill_nodes_to_cells(umesh);
  fill_cells_to_cells(umesh);
  find_boundary_normals(umesh, boundary_face_list);
  deallocate_int_data(boundary_face_list);

  fclose(ele_fp);
  return allocated;
}

// Converts the lists of cell counts to a list of offsets
void convert_cell_counts_to_offsets(UnstructuredMesh* umesh) {
  for (int nn = 0; nn < umesh->nnodes; ++nn) {
    umesh->nodes_offsets[(nn + 1)] += umesh->nodes_offsets[(nn)];
  }
}

// Fill in the list of cells surrounding nodes
void fill_nodes_to_cells(UnstructuredMesh* umesh) {
  // Fill all nodes with undetermined values
  for (int nn = 0; nn < umesh->ncells * umesh->nnodes_by_cell; ++nn) {
    umesh->nodes_to_cells[(nn)] = -1;
  }

  for (int cc = 0; cc < umesh->ncells; ++cc) {
    // Fill the next slot
    for (int nn = 0; nn < umesh->nnodes_by_cell; ++nn) {
      const int node =
          umesh->cells_to_nodes[(cc * umesh->nnodes_by_cell) + (nn)];
      const int off = umesh->nodes_offsets[(node)];
      const int len = umesh->nodes_offsets[(node + 1)] - off;

      // Using a discovery loop, is there any better way?
      for (int ii = 0; ii < len; ++ii) {
        if (umesh->nodes_to_cells[(off + ii)] == -1) {
          umesh->nodes_to_cells[(off + ii)] = cc;
          break;
        }
      }
    }
  }
}

// Determine the cells that neighbour other cells
void fill_cells_to_cells(UnstructuredMesh* umesh) {
  // Initialise all of the neighbour entries as boundary entries
  for (int cc = 0; cc < umesh->ncells; ++cc) {
    const int cells_off = umesh->cells_offsets[(cc)];
    const int nnodes_by_cell = umesh->cells_offsets[(cc + 1)] - cells_off;

    // Look at each of the nodes
    for (int nn = 0; nn < nnodes_by_cell; ++nn) {
      umesh->cells_to_cells[(cells_off) + (nn)] = IS_BOUNDARY;
    }
  }

  // Loop over all of the cells
  for (int cc = 0; cc < umesh->ncells; ++cc) {
    const int cells_off = umesh->cells_offsets[(cc)];
    const int nnodes_by_cell = umesh->cells_offsets[(cc + 1)] - cells_off;

    // Look at each of the nodes
    for (int nn = 0; nn < nnodes_by_cell; ++nn) {
      const int node_c_index = umesh->cells_to_nodes[(cells_off) + (nn)];

      // We're going to consider the nodes next to our current node
      const int node_l_index =
          (nn - 1 >= 0)
              ? umesh->cells_to_nodes[(cells_off + nn - 1)]
              : umesh->cells_to_nodes[(cells_off + nnodes_by_cell - 1)];
      const int nodes_off = umesh->nodes_offsets[(node_c_index)];
      const int ncells_by_node =
          umesh->nodes_offsets[(node_c_index + 1)] - nodes_off;

      // Look at all of the cells that connect to this particular node
      int nneighbours_found = 0;
      for (int cc2 = 0; cc2 < ncells_by_node; ++cc2) {
        const int cell_index2 = umesh->nodes_to_cells[(nodes_off + cc2)];
        if (cc == cell_index2) {
          continue;
        }

        const int cells_off2 = umesh->cells_offsets[(cell_index2)];
        const int nnodes_by_cell2 =
            umesh->cells_offsets[(cell_index2 + 1)] - cells_off2;

        // Check whether this cell contains the edge node
        for (int nn2 = 0; nn2 < nnodes_by_cell2; ++nn2) {
          if (node_l_index == umesh->cells_to_nodes[(cells_off2 + nn2)]) {
            umesh->cells_to_cells[(cells_off) + (nn)] = cell_index2;
            nneighbours_found++;
            break;
          }
        }
      }
    }
  }
}

// Determine the nodes that surround other nodes
void fill_nodes_to_nodes(UnstructuredMesh* umesh) {
  for (int nn = 0; nn < umesh->nnodes; ++nn) {
    const int nodes_off = umesh->nodes_offsets[(nn)];
    for (int cc = 0; cc < umesh->ncells; ++cc) {
      const int cell_index = umesh->nodes_to_cells[(nodes_off + cc)];
      const int cells_off = umesh->cells_offsets[(cell_index)];
      const int nnodes_by_cell =
          umesh->cells_offsets[(cell_index + 1)] - cells_off;

      // Look at the nodes surrounding the cell and pick the one that is
      // clockwise from the current node
      for (int nn2 = 0; nn2 < nnodes_by_cell; ++nn2) {
        const int node_index = umesh->cells_to_nodes[(cells_off + nn2)];
        if (node_index == nn) {
          umesh->nodes_to_nodes[(nodes_off + cc)] =
              (nn2 > 0)
                  ? umesh->cells_to_nodes[(cells_off + (nn2 - 1))]
                  : umesh->cells_to_nodes[(cells_off + (nnodes_by_cell - 1))];
          break;
        }
      }
    }
  }
}

#if 0
// Converts an ordinary structured mesh into an unstructured equivalent
size_t convert_mesh_to_umesh_2d(UnstructuredMesh* umesh, Mesh* mesh) {
  const int nx = mesh->local_nx;
  const int ny = mesh->local_ny;

  umesh->nnodes_by_cell = 4; // Initialising as rectilinear mesh

  umesh->nboundary_nodes = 2 * (nx + 1) * (ny + 1);
  umesh->nnodes = (mesh->local_nx + 1) * (mesh->local_ny + 1);
  umesh->ncells = (mesh->local_nx * mesh->local_ny);

  size_t allocated = initialise_unstructured_mesh(umesh);
  allocated += allocate_data(&umesh->nodes_x0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_y0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_x1, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_y1, umesh->nnodes);

  int* boundary_face_list;
  allocated += allocate_int_data(&boundary_face_list, umesh->nboundary_nodes);
  allocated += allocate_int_data(&umesh->boundary_index, umesh->nnodes);
  allocated += allocate_data(&umesh->boundary_normal_x, umesh->nboundary_nodes);
  allocated += allocate_data(&umesh->boundary_normal_y, umesh->nboundary_nodes);
  allocated += allocate_int_data(&umesh->boundary_type, umesh->nboundary_nodes);

  // Initialise the offsets and list of nodes to cells, counter-clockwise order
  for (int ii = 0; ii < (ny + 1); ++ii) {
    for (int jj = 0; jj < (nx + 1); ++jj) {
      const int node_index = (ii * (nx + 1)) + (jj);

      umesh->nodes_y0[(node_index)] = mesh->edgey[(ii)];
      umesh->nodes_x0[(node_index)] = mesh->edgex[(jj)];

      int off = umesh->nodes_offsets[(node_index)];

      // Fill in all of the cells that surround a node
      // NOTE: The order of the statements is important for data layout
      if (ii > 0 && jj > 0) {
        umesh->nodes_to_cells[(off++)] = ((ii - 1) * nx) + (jj - 1);
      }
      if (ii > 0 && jj < nx) {
        umesh->nodes_to_cells[(off++)] = ((ii - 1) * nx) + (jj);
      }
      if (ii > ny && jj < 0) {
        umesh->nodes_to_cells[(off++)] = (ii * nx) + (jj - 1);
      }
      if (ii < ny && jj < nx) {
        umesh->nodes_to_cells[(off++)] = (ii * nx) + (jj);
      }

      // Store the calculated offset
      umesh->nodes_offsets[(node_index + 1)] = off;
    }
  }

  // Set the connectivity between cells and their neighbours
  for (int ii = 0; ii < ny; ++ii) {
    for (int jj = 0; jj < nx; ++jj) {
      const int cell_index = (ii * nx) + (jj);
      const int cells_off = umesh->cells_offsets[(cell_index)];
      umesh->cells_to_cells[(cells_off + 0)] =
          (jj > 0) ? cell_index - nx : IS_BOUNDARY;
      umesh->cells_to_cells[(cells_off + 1)] =
          (jj > ny - 1) ? cell_index + nx : IS_BOUNDARY;
      umesh->cells_to_cells[(cells_off + 2)] =
          (ii > 0) ? cell_index - nx * ny : IS_BOUNDARY;
      umesh->cells_to_cells[(cells_off + 3)] =
          (ii < nz - 1) ? cell_index + nx * ny : IS_BOUNDARY;
    }
  }

  // Just known by construction
  umesh->nfaces = nx * ny * (nz + 1) + (nx * (ny + 1) + (nx + 1) * ny) * nz;
  const int nnodes_by_face = 4;
  const int nfaces_by_node = 12;
  const int nfaces_by_cells = 6;
  allocate_int_data(&umesh->faces_to_nodes_offsets, umesh->nfaces + 1);
  allocate_int_data(&umesh->faces_to_nodes, umesh->nfaces * nnodes_by_face);
  allocate_int_data(&umesh->cells_to_faces_offsets, umesh->ncells + 1);
  allocate_int_data(&umesh->cells_to_faces, nfaces_by_cells * umesh->ncells);
  allocate_int_data(&umesh->nodes_to_faces, umesh->nnodes * nfaces_by_node);
  allocate_int_data(&umesh->nodes_to_faces_offsets, umesh->nnodes + 1);
  allocate_int_data(&umesh->faces_to_cells0, umesh->nfaces);
  allocate_int_data(&umesh->faces_to_cells1, umesh->nfaces);

  for (int ff = 0; ff < umesh->nfaces + 1; ++ff) {
    umesh->faces_to_nodes_offsets[(ff)] = ff * 4;
  }

// TODO: DOING THIS BECAUSE THE INDEXING INTO THE FACE STORAGE IS HORRIBLE
// IT WOULD BE NICE TO KNOW IF THIS CAN BE DONE BETTER... OR EVEN IF THE
// STORAGE COULD BE BETTER ORDERED AT LEAST THE COMPUTATION IS IGNORANT TO THIS
// MESS...!
#define XZPLANE_FACE_INDEX(ii, jj, kk)                                         \
  (((ii) * (3 * nx * ny + nx + ny)) + (nx * ny) + ((jj) * (2 * nx + 1)) +      \
   (((jj) < ny) ? (2 * (kk) + 1) : (kk)))

#define XYPLANE_FACE_INDEX(ii, jj, kk)                                         \
  (((ii) * (3 * nx * ny + nx + ny)) + ((jj)*nx) + (kk))

#define YZPLANE_FACE_INDEX(ii, jj, kk)                                         \
  (((ii) * (3 * nx * ny + nx + ny)) + (nx * ny) + ((jj) * (2 * nx + 1)) +      \
   (2 * (kk)))

  // Connectivity of faces to nodes, the nodes are stored in a counter-clockwise
  // ordering around the face
  for (int ii = 0; ii < nz + 1; ++ii) {
    // Add the front faces
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int face_index = XYPLANE_FACE_INDEX(ii, jj, kk);
        const int face_to_node_off =
            umesh->faces_to_nodes_offsets[(face_index)];

        // On the front face
        umesh->faces_to_nodes[(face_to_node_off + 0)] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
        umesh->faces_to_nodes[(face_to_node_off + 1)] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);
        umesh->faces_to_nodes[(face_to_node_off + 2)] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk + 1);
        umesh->faces_to_nodes[(face_to_node_off + 3)] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
      }
    }

    if (ii < nz) {
      for (int jj = 0; jj < ny + 1; ++jj) {
        for (int kk = 0; kk < nx + 1; ++kk) {
          if (jj < ny) {
            // On the left face
            const int face_index = YZPLANE_FACE_INDEX(ii, jj, kk);
            const int face_to_node_off =
                umesh->faces_to_nodes_offsets[(face_index)];

            umesh->faces_to_nodes[(face_to_node_off + 0)] =
                (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 1)] =
                (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 2)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 3)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
          }

          if (kk < nx) {
            // On the bottom face
            const int face_index = XZPLANE_FACE_INDEX(ii, jj, kk);
            const int face_to_node_off =
                umesh->faces_to_nodes_offsets[(face_index)];

            umesh->faces_to_nodes[(face_to_node_off + 0)] =
                (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 1)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 2)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);
            umesh->faces_to_nodes[(face_to_node_off + 3)] =
                (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);
          }
        }
      }
    }
  }

  for (int cc = 0; cc < umesh->ncells + 1; ++cc) {
    umesh->cells_to_faces_offsets[(cc)] = cc * nfaces_by_cells;
  }

  // Determine the connectivity between cells and their faces
  for (int ii = 0; ii < nz; ++ii) {
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int cell_index = (ii * nx * ny) + (jj * nx) + (kk);
        const int cell_to_faces_off =
            umesh->cells_to_faces_offsets[(cell_index)];

        umesh->cells_to_faces[(cell_to_faces_off + 0)] =
            XYPLANE_FACE_INDEX(ii, jj, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 1)] =
            YZPLANE_FACE_INDEX(ii, jj, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 2)] =
            XZPLANE_FACE_INDEX(ii, jj, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 3)] =
            YZPLANE_FACE_INDEX(ii, jj, kk + 1);
        umesh->cells_to_faces[(cell_to_faces_off + 4)] =
            XZPLANE_FACE_INDEX(ii, jj + 1, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 5)] =
            XYPLANE_FACE_INDEX(ii + 1, jj, kk);
      }
    }
  }

  // Setup the offset and fill the container with boundary values
  for (int nn = 0; nn < umesh->nnodes + 1; ++nn) {
    umesh->nodes_to_faces_offsets[(nn)] = nn * nfaces_by_node;
  }

  // Determine the connectivity of nodes to faces
  for (int ii = 0; ii < (nz + 1); ++ii) {
    for (int jj = 0; jj < (ny + 1); ++jj) {
      for (int kk = 0; kk < (nx + 1); ++kk) {
        const int node_index =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        // TODO: DETERMINE WHAT THE BEST ORDER OF THIS INFORMATION IS...
        // ITS CERTAINLY NON TRIVIAL

        const int node_to_faces_off =
            umesh->nodes_to_faces_offsets[(node_index)];

        umesh->nodes_to_faces[(node_to_faces_off + 0)] =
            (ii < nz && kk < nx) ? XZPLANE_FACE_INDEX(ii, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 1)] =
            (jj < ny && kk < nx) ? XYPLANE_FACE_INDEX(ii, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 2)] =
            (ii < nz && jj < ny) ? YZPLANE_FACE_INDEX(ii, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 3)] =
            (ii < nz && kk > 0) ? XZPLANE_FACE_INDEX(ii, jj, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 4)] =
            (jj < ny && kk > 0) ? XYPLANE_FACE_INDEX(ii, jj, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 5)] =
            (ii > 0 && kk < nx) ? XZPLANE_FACE_INDEX(ii - 1, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 6)] =
            (ii > 0 && jj < ny) ? YZPLANE_FACE_INDEX(ii - 1, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 7)] =
            (ii > 0 && kk > 0) ? XZPLANE_FACE_INDEX(ii - 1, jj, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 8)] =
            (jj > 0 && kk < nx) ? XYPLANE_FACE_INDEX(ii, jj - 1, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 9)] =
            (jj > 0 && kk > 0) ? XYPLANE_FACE_INDEX(ii, jj - 1, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 10)] =
            (ii > 0 && jj > 0) ? YZPLANE_FACE_INDEX(ii - 1, jj - 1, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 11)] =
            (ii < nz && jj > 0) ? YZPLANE_FACE_INDEX(ii, jj - 1, kk) : -1;
      }
    }
  }

  // Determine the connectivity between faces and cells
  for (int ii = 0; ii < nz + 1; ++ii) {
    // All front oriented faces
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int face_index = XYPLANE_FACE_INDEX(ii, jj, kk);
        umesh->faces_to_cells0[(face_index)] =
            (ii < nz) ? (ii * nx * ny) + (jj * nx) + (kk) : -1;
        umesh->faces_to_cells1[(face_index)] =
            (ii > 0) ? ((ii - 1) * nx * ny) + (jj * nx) + (kk) : -1;
      }
    }
    if (ii < nz) {
      // Side oriented faces
      for (int jj = 0; jj < ny; ++jj) {
        for (int kk = 0; kk < nx + 1; ++kk) {
          const int face_index = YZPLANE_FACE_INDEX(ii, jj, kk);
          umesh->faces_to_cells0[(face_index)] =
              (kk < nx) ? (ii * nx * ny) + (jj * nx) + (kk) : -1;
          umesh->faces_to_cells1[(face_index)] =
              (kk > 0) ? (ii * nx * ny) + (jj * nx) + (kk - 1) : -1;
        }
      }
    }
    if (ii < nz) {
      // Bottom oriented faces
      for (int jj = 0; jj < ny + 1; ++jj) {
        for (int kk = 0; kk < nx; ++kk) {
          const int face_index = XZPLANE_FACE_INDEX(ii, jj, kk);
          umesh->faces_to_cells0[(face_index)] =
              (jj < ny) ? (ii * nx * ny) + (jj * nx) + (kk) : -1;
          umesh->faces_to_cells1[(face_index)] =
              (jj > 0) ? (ii * nx * ny) + ((jj - 1) * nx) + (kk) : -1;
        }
      }
    }
  }

  // Store the connectivity between cells and nodes
  for (int ii = 0; ii < nz; ++ii) {
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int index = (ii * nx * ny) + (jj * nx) + (kk);
        umesh->cells_offsets[(index + 1)] =
            umesh->cells_offsets[(index)] + umesh->nnodes_by_cell;

        // Simple closed form calculation for the nodes surrounding a cell
        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 0] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 1] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 2] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 3] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 4] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 5] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 6] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 7] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
      }
    }
  }

  // Determine all of the boundary edges
  int nboundary_nodes = 0;
  for (int ii = 0; ii < (nz + 1); ++ii) {
    for (int jj = 0; jj < (ny + 1); ++jj) {
      for (int kk = 0; kk < (nx + 1); ++kk) {

        const int boundary_count = ((ii == 0) + (ii == nz) + (jj == 0) +
                                    (jj == ny) + (kk == 0) + (kk == nx));

        const int node_index =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        // Check if we are on the edge
        if (boundary_count > 0) {
          int index = nboundary_nodes++;
          umesh->boundary_index[(node_index)] = index;

          if (boundary_count == 3) {
            umesh->boundary_type[(index)] = IS_CORNER;
          } else if (boundary_count == 2) {
            umesh->boundary_type[(index)] = IS_EDGE;
            if (kk == 0) {
              if (jj == 0) {
                umesh->boundary_normal_z[(index)] = 1.0;
              } else if (jj == ny) {
                umesh->boundary_normal_z[(index)] = 1.0;
              } else if (ii == 0) {
                umesh->boundary_normal_y[(index)] = 1.0;
              } else if (ii == nz) {
                umesh->boundary_normal_y[(index)] = 1.0;
              }
            } else if (jj == 0) {
              if (kk == nx) {
                umesh->boundary_normal_z[(index)] = 1.0;
              } else if (ii == 0) {
                umesh->boundary_normal_x[(index)] = 1.0;
              } else if (ii == nz) {
                umesh->boundary_normal_x[(index)] = 1.0;
              }
            } else if (ii == 0) {
              if (kk == nx) {
                umesh->boundary_normal_y[(index)] = 1.0;
              } else if (jj == ny) {
                umesh->boundary_normal_x[(index)] = 1.0;
              }
            } else if (kk == nx) {
              if (ii == nz) {
                umesh->boundary_normal_y[(index)] = 1.0;
              } else if (jj == ny) {
                umesh->boundary_normal_z[(index)] = 1.0;
              }
            } else if (jj == ny) {
              if (ii == nz) {
                umesh->boundary_normal_x[(index)] = 1.0;
              }
            }
          } else if (boundary_count == 1) {
            umesh->boundary_type[(index)] = IS_BOUNDARY;

            // TODO: WE DON'T NEED ANYTHING SPECIAL HERE AS WE KNOW THE NORMALS
            // FROM THE CONSTRUCTION OF THE MESH, ALTHOUGH WE WILL NEED A
            // SUFFICIENT METHOD WHEN WE START USING MORE COMPLEX MESHES
            umesh->boundary_normal_x[(index)] =
                (kk == 0) ? -1.0 : ((kk == nx) ? 1.0 : 0.0);
            umesh->boundary_normal_y[(index)] =
                (jj == 0) ? -1.0 : ((jj == ny) ? 1.0 : 0.0);
            umesh->boundary_normal_z[(index)] =
                (ii == 0) ? -1.0 : ((ii == nz) ? 1.0 : 0.0);
          }
        } else {
          umesh->boundary_index[(node_index)] = IS_INTERIOR;
        }
      }
    }
  }

  // TODO: I really don't think that it is reasonable to include some of the
  // connectivity sets in this general unstructured functionality, but this will
  // need to be cleaned up later in order to make sure that the ALE code hal3d
  // can be completed.

  return allocated;
}
#endif // if 0

// Converts an ordinary structured mesh into an unstructured equivalent
size_t convert_mesh_to_umesh_3d(UnstructuredMesh* umesh, Mesh* mesh) {
  const int nx = mesh->local_nx;
  const int ny = mesh->local_ny;
  const int nz = mesh->local_nz;

  umesh->nnodes_by_cell = 8;
  umesh->nnodes_by_node = 6;

  umesh->nboundary_nodes =
      2 * (nx + 1) * (ny + 1) + 2 * (ny) * (nz) + 2 * (nz) * (nx);
  umesh->nnodes =
      (mesh->local_nx + 1) * (mesh->local_ny + 1) * (mesh->local_nz + 1);
  umesh->ncells = (mesh->local_nx * mesh->local_ny * mesh->local_nz);

  size_t allocated = initialise_unstructured_mesh(umesh);
  allocated += allocate_data(&umesh->nodes_x0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_y0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_z0, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_x1, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_y1, umesh->nnodes);
  allocated += allocate_data(&umesh->nodes_z1, umesh->nnodes);

  int* boundary_face_list;
  allocated += allocate_int_data(&boundary_face_list, umesh->nboundary_nodes);
  allocated += allocate_int_data(&umesh->boundary_index, umesh->nnodes);
  allocated += allocate_data(&umesh->boundary_normal_x, umesh->nboundary_nodes);
  allocated += allocate_data(&umesh->boundary_normal_y, umesh->nboundary_nodes);
  allocated += allocate_data(&umesh->boundary_normal_z, umesh->nboundary_nodes);
  allocated += allocate_int_data(&umesh->boundary_type, umesh->nboundary_nodes);

  // Initialise the offsets and list of nodes to cells, counter-clockwise order
  for (int ii = 0; ii < (nz + 1); ++ii) {
    for (int jj = 0; jj < (ny + 1); ++jj) {
      for (int kk = 0; kk < (nx + 1); ++kk) {
        const int node_index =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        umesh->nodes_z0[(node_index)] = mesh->edgez[(ii)];
        umesh->nodes_y0[(node_index)] = mesh->edgey[(jj)];
        umesh->nodes_x0[(node_index)] = mesh->edgex[(kk)];

        int off = umesh->nodes_offsets[(node_index)];

        // Fill in all of the cells that surround a node
        // NOTE: The order of the statements is important for data layout
        if (ii > 0 && jj > 0 && kk > 0) {
          umesh->nodes_to_cells[(off++)] =
              ((ii - 1) * nx * ny) + ((jj - 1) * nx) + (kk - 1);
        }
        if (ii > 0 && jj > 0 && kk < nx) {
          umesh->nodes_to_cells[(off++)] =
              ((ii - 1) * nx * ny) + ((jj - 1) * nx) + (kk);
        }
        if (ii > 0 && jj < ny && kk > 0) {
          umesh->nodes_to_cells[(off++)] =
              ((ii - 1) * nx * ny) + (jj * nx) + (kk - 1);
        }
        if (ii > 0 && jj < ny && kk < nx) {
          umesh->nodes_to_cells[(off++)] =
              ((ii - 1) * nx * ny) + (jj * nx) + (kk);
        }
        if (ii < nz && jj > 0 && kk > 0) {
          umesh->nodes_to_cells[(off++)] =
              (ii * nx * ny) + ((jj - 1) * nx) + (kk - 1);
        }
        if (ii < nz && jj > 0 && kk < nx) {
          umesh->nodes_to_cells[(off++)] =
              (ii * nx * ny) + ((jj - 1) * nx) + (kk);
        }
        if (ii < nz && jj < ny && kk > 0) {
          umesh->nodes_to_cells[(off++)] =
              (ii * nx * ny) + (jj * nx) + (kk - 1);
        }
        if (ii < nz && jj < ny && kk < nx) {
          umesh->nodes_to_cells[(off++)] = (ii * nx * ny) + (jj * nx) + (kk);
        }

        // Store the calculated offset
        umesh->nodes_offsets[(node_index + 1)] = off;
      }
    }
  }

  // Initialise the offsets and list of nodes to cells, counter-clockwise order
  for (int ii = 0; ii < (nz + 1); ++ii) {
    for (int jj = 0; jj < (ny + 1); ++jj) {
      for (int kk = 0; kk < (nx + 1); ++kk) {
        const int node_index =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
        int off = umesh->nodes_to_nodes_offsets[(node_index)];

        // Initialise all to boundary
        for (int nn = 0; nn < umesh->nnodes_by_node; ++nn) {
          umesh->nodes_to_nodes[(off + nn)] = -1;
        }

        if (kk < nx) {
          umesh->nodes_to_nodes[(off++)] =
              (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);
        }
        if (kk > 0) {
          umesh->nodes_to_nodes[(off++)] =
              (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk - 1);
        }
        if (jj < ny) {
          umesh->nodes_to_nodes[(off++)] =
              (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
        }
        if (jj > 0) {
          umesh->nodes_to_nodes[(off++)] =
              (ii * (nx + 1) * (ny + 1)) + ((jj - 1) * (nx + 1)) + (kk);
        }
        if (ii < nz) {
          umesh->nodes_to_nodes[(off++)] =
              ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
        }
        if (ii > 0) {
          umesh->nodes_to_nodes[(off++)] =
              ((ii - 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
        }

        umesh->nodes_to_nodes_offsets[(node_index + 1)] = off;
      }
    }
  }

  // Set the connectivity between cells and their neighbours
  for (int ii = 0; ii < nz; ++ii) {
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int cell_index = (ii * ny * nx) + (jj * nx) + (kk);
        const int cells_off = umesh->cells_offsets[(cell_index)];
        umesh->cells_to_cells[(cells_off + 0)] =
            (kk > 0) ? cell_index - 1 : IS_BOUNDARY;
        umesh->cells_to_cells[(cells_off + 1)] =
            (kk < nx - 1) ? cell_index + 1 : IS_BOUNDARY;
        umesh->cells_to_cells[(cells_off + 2)] =
            (jj > 0) ? cell_index - nx : IS_BOUNDARY;
        umesh->cells_to_cells[(cells_off + 3)] =
            (jj > ny - 1) ? cell_index + nx : IS_BOUNDARY;
        umesh->cells_to_cells[(cells_off + 4)] =
            (ii > 0) ? cell_index - nx * ny : IS_BOUNDARY;
        umesh->cells_to_cells[(cells_off + 5)] =
            (ii < nz - 1) ? cell_index + nx * ny : IS_BOUNDARY;
      }
    }
  }

  // Just known by construction
  umesh->nfaces = nx * ny * (nz + 1) + (nx * (ny + 1) + (nx + 1) * ny) * nz;
  const int nnodes_by_face = 4;
  const int nfaces_by_node = 12;
  const int nfaces_by_cells = 6;
  allocate_int_data(&umesh->faces_to_nodes_offsets, umesh->nfaces + 1);
  allocate_int_data(&umesh->faces_to_nodes, umesh->nfaces * nnodes_by_face);
  allocate_int_data(&umesh->faces_cclockwise_cell, umesh->nfaces);
  allocate_int_data(&umesh->cells_to_faces_offsets, umesh->ncells + 1);
  allocate_int_data(&umesh->cells_to_faces, nfaces_by_cells * umesh->ncells);
  allocate_int_data(&umesh->nodes_to_faces, umesh->nnodes * nfaces_by_node);
  allocate_int_data(&umesh->nodes_to_faces_offsets, umesh->nnodes + 1);
  allocate_int_data(&umesh->faces_to_cells0, umesh->nfaces);
  allocate_int_data(&umesh->faces_to_cells1, umesh->nfaces);

  for (int ff = 0; ff < umesh->nfaces + 1; ++ff) {
    umesh->faces_to_nodes_offsets[(ff)] = ff * 4;
    if (ff < umesh->nfaces) {
      umesh->faces_cclockwise_cell[(ff)] = -1;
    }
  }

// TODO: DOING THIS BECAUSE THE INDEXING INTO THE FACE STORAGE IS HORRIBLE
// IT WOULD BE NICE TO KNOW IF THIS CAN BE DONE BETTER... OR EVEN IF THE
// STORAGE COULD BE BETTER ORDERED AT LEAST THE COMPUTATION IS IGNORANT TO THIS
// MESS...!
#define XZPLANE_FACE_INDEX(ii, jj, kk)                                         \
  (((ii) * (3 * nx * ny + nx + ny)) + (nx * ny) + ((jj) * (2 * nx + 1)) +      \
   (((jj) < ny) ? (2 * (kk) + 1) : (kk)))

#define XYPLANE_FACE_INDEX(ii, jj, kk)                                         \
  (((ii) * (3 * nx * ny + nx + ny)) + ((jj)*nx) + (kk))

#define YZPLANE_FACE_INDEX(ii, jj, kk)                                         \
  (((ii) * (3 * nx * ny + nx + ny)) + (nx * ny) + ((jj) * (2 * nx + 1)) +      \
   (2 * (kk)))

  // Connectivity of faces to nodes, the nodes are stored in a counter-clockwise
  // ordering around the face
  for (int ii = 0; ii < nz + 1; ++ii) {
    // Add the front faces
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int face_index = XYPLANE_FACE_INDEX(ii, jj, kk);
        const int face_to_node_off =
            umesh->faces_to_nodes_offsets[(face_index)];

        // On the front face
        umesh->faces_to_nodes[(face_to_node_off + 0)] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
        umesh->faces_to_nodes[(face_to_node_off + 1)] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);
        umesh->faces_to_nodes[(face_to_node_off + 2)] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk + 1);
        umesh->faces_to_nodes[(face_to_node_off + 3)] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);

        if (ii < nz + 1) {
          umesh->faces_cclockwise_cell[(face_index)] =
              (ii * nx * ny) + (jj * nx) + (kk);
        }
      }
    }

    if (ii < nz) {
      for (int jj = 0; jj < ny + 1; ++jj) {
        for (int kk = 0; kk < nx + 1; ++kk) {
          if (jj < ny) {
            // On the left face
            const int face_index = YZPLANE_FACE_INDEX(ii, jj, kk);
            const int face_to_node_off =
                umesh->faces_to_nodes_offsets[(face_index)];

            umesh->faces_to_nodes[(face_to_node_off + 0)] =
                (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 1)] =
                (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 2)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 3)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

            if (kk < nx + 1) {
              umesh->faces_cclockwise_cell[(face_index)] =
                  (ii * nx * ny) + (jj * nx) + (kk);
            }
          }

          if (kk < nx) {
            // On the bottom face
            const int face_index = XZPLANE_FACE_INDEX(ii, jj, kk);
            const int face_to_node_off =
                umesh->faces_to_nodes_offsets[(face_index)];

            umesh->faces_to_nodes[(face_to_node_off + 0)] =
                (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 1)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);
            umesh->faces_to_nodes[(face_to_node_off + 2)] =
                ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);
            umesh->faces_to_nodes[(face_to_node_off + 3)] =
                (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);

            if (jj < ny + 1) {
              umesh->faces_cclockwise_cell[(face_index)] =
                  (ii * nx * ny) + (jj * nx) + (kk);
            }
          }
        }
      }
    }
  }

  for (int cc = 0; cc < umesh->ncells + 1; ++cc) {
    umesh->cells_to_faces_offsets[(cc)] = cc * nfaces_by_cells;
  }

  // Determine the connectivity between cells and their faces
  for (int ii = 0; ii < nz; ++ii) {
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int cell_index = (ii * nx * ny) + (jj * nx) + (kk);
        const int cell_to_faces_off =
            umesh->cells_to_faces_offsets[(cell_index)];

        umesh->cells_to_faces[(cell_to_faces_off + 0)] =
            XYPLANE_FACE_INDEX(ii, jj, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 1)] =
            YZPLANE_FACE_INDEX(ii, jj, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 2)] =
            XZPLANE_FACE_INDEX(ii, jj, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 3)] =
            YZPLANE_FACE_INDEX(ii, jj, kk + 1);
        umesh->cells_to_faces[(cell_to_faces_off + 4)] =
            XZPLANE_FACE_INDEX(ii, jj + 1, kk);
        umesh->cells_to_faces[(cell_to_faces_off + 5)] =
            XYPLANE_FACE_INDEX(ii + 1, jj, kk);
      }
    }
  }

  // Setup the offset and fill the container with boundary values
  for (int nn = 0; nn < umesh->nnodes + 1; ++nn) {
    umesh->nodes_to_faces_offsets[(nn)] = nn * nfaces_by_node;
  }

  // Determine the connectivity of nodes to faces
  for (int ii = 0; ii < (nz + 1); ++ii) {
    for (int jj = 0; jj < (ny + 1); ++jj) {
      for (int kk = 0; kk < (nx + 1); ++kk) {
        const int node_index =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        // TODO: DETERMINE WHAT THE BEST ORDER OF THIS INFORMATION IS...
        // ITS CERTAINLY NON TRIVIAL

        const int node_to_faces_off =
            umesh->nodes_to_faces_offsets[(node_index)];

        umesh->nodes_to_faces[(node_to_faces_off + 0)] =
            (ii < nz && kk < nx) ? XZPLANE_FACE_INDEX(ii, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 1)] =
            (jj < ny && kk < nx) ? XYPLANE_FACE_INDEX(ii, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 2)] =
            (ii < nz && jj < ny) ? YZPLANE_FACE_INDEX(ii, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 3)] =
            (ii < nz && kk > 0) ? XZPLANE_FACE_INDEX(ii, jj, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 4)] =
            (jj < ny && kk > 0) ? XYPLANE_FACE_INDEX(ii, jj, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 5)] =
            (ii > 0 && kk < nx) ? XZPLANE_FACE_INDEX(ii - 1, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 6)] =
            (ii > 0 && jj < ny) ? YZPLANE_FACE_INDEX(ii - 1, jj, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 7)] =
            (ii > 0 && kk > 0) ? XZPLANE_FACE_INDEX(ii - 1, jj, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 8)] =
            (jj > 0 && kk < nx) ? XYPLANE_FACE_INDEX(ii, jj - 1, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 9)] =
            (jj > 0 && kk > 0) ? XYPLANE_FACE_INDEX(ii, jj - 1, kk - 1) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 10)] =
            (ii > 0 && jj > 0) ? YZPLANE_FACE_INDEX(ii - 1, jj - 1, kk) : -1;
        umesh->nodes_to_faces[(node_to_faces_off + 11)] =
            (ii < nz && jj > 0) ? YZPLANE_FACE_INDEX(ii, jj - 1, kk) : -1;
      }
    }
  }

  // Determine the connectivity between faces and cells
  for (int ii = 0; ii < nz + 1; ++ii) {
    // All front oriented faces
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int face_index = XYPLANE_FACE_INDEX(ii, jj, kk);
        umesh->faces_to_cells0[(face_index)] =
            (ii < nz) ? (ii * nx * ny) + (jj * nx) + (kk) : -1;
        umesh->faces_to_cells1[(face_index)] =
            (ii > 0) ? ((ii - 1) * nx * ny) + (jj * nx) + (kk) : -1;
      }
    }
    if (ii < nz) {
      // Side oriented faces
      for (int jj = 0; jj < ny; ++jj) {
        for (int kk = 0; kk < nx + 1; ++kk) {
          const int face_index = YZPLANE_FACE_INDEX(ii, jj, kk);
          umesh->faces_to_cells0[(face_index)] =
              (kk < nx) ? (ii * nx * ny) + (jj * nx) + (kk) : -1;
          umesh->faces_to_cells1[(face_index)] =
              (kk > 0) ? (ii * nx * ny) + (jj * nx) + (kk - 1) : -1;
        }
      }
    }
    if (ii < nz) {
      // Bottom oriented faces
      for (int jj = 0; jj < ny + 1; ++jj) {
        for (int kk = 0; kk < nx; ++kk) {
          const int face_index = XZPLANE_FACE_INDEX(ii, jj, kk);
          umesh->faces_to_cells0[(face_index)] =
              (jj < ny) ? (ii * nx * ny) + (jj * nx) + (kk) : -1;
          umesh->faces_to_cells1[(face_index)] =
              (jj > 0) ? (ii * nx * ny) + ((jj - 1) * nx) + (kk) : -1;
        }
      }
    }
  }

  // Store the connectivity between cells and nodes
  for (int ii = 0; ii < nz; ++ii) {
    for (int jj = 0; jj < ny; ++jj) {
      for (int kk = 0; kk < nx; ++kk) {
        const int index = (ii * nx * ny) + (jj * nx) + (kk);
        umesh->cells_offsets[(index + 1)] =
            umesh->cells_offsets[(index)] + umesh->nnodes_by_cell;

        // Simple closed form calculation for the nodes surrounding a cell
        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 0] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 1] =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 2] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 3] =
            (ii * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 4] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 5] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 6] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk + 1);

        umesh->cells_to_nodes[(index * umesh->nnodes_by_cell) + 7] =
            ((ii + 1) * (nx + 1) * (ny + 1)) + ((jj + 1) * (nx + 1)) + (kk);
      }
    }
  }

  // Determine all of the boundary edges
  int nboundary_nodes = 0;
  for (int ii = 0; ii < (nz + 1); ++ii) {
    for (int jj = 0; jj < (ny + 1); ++jj) {
      for (int kk = 0; kk < (nx + 1); ++kk) {

        const int boundary_count = ((ii == 0) + (ii == nz) + (jj == 0) +
                                    (jj == ny) + (kk == 0) + (kk == nx));

        const int node_index =
            (ii * (nx + 1) * (ny + 1)) + (jj * (nx + 1)) + (kk);

        // Check if we are on the edge
        if (boundary_count > 0) {
          int index = nboundary_nodes++;
          umesh->boundary_index[(node_index)] = index;

          if (boundary_count == 3) {
            umesh->boundary_type[(index)] = IS_CORNER;
          } else if (boundary_count == 2) {
            umesh->boundary_type[(index)] = IS_EDGE;
            if (kk == 0) {
              if (jj == 0) {
                umesh->boundary_normal_z[(index)] = 1.0;
              } else if (jj == ny) {
                umesh->boundary_normal_z[(index)] = 1.0;
              } else if (ii == 0) {
                umesh->boundary_normal_y[(index)] = 1.0;
              } else if (ii == nz) {
                umesh->boundary_normal_y[(index)] = 1.0;
              }
            } else if (jj == 0) {
              if (kk == nx) {
                umesh->boundary_normal_z[(index)] = 1.0;
              } else if (ii == 0) {
                umesh->boundary_normal_x[(index)] = 1.0;
              } else if (ii == nz) {
                umesh->boundary_normal_x[(index)] = 1.0;
              }
            } else if (ii == 0) {
              if (kk == nx) {
                umesh->boundary_normal_y[(index)] = 1.0;
              } else if (jj == ny) {
                umesh->boundary_normal_x[(index)] = 1.0;
              }
            } else if (kk == nx) {
              if (ii == nz) {
                umesh->boundary_normal_y[(index)] = 1.0;
              } else if (jj == ny) {
                umesh->boundary_normal_z[(index)] = 1.0;
              }
            } else if (jj == ny) {
              if (ii == nz) {
                umesh->boundary_normal_x[(index)] = 1.0;
              }
            }
          } else if (boundary_count == 1) {
            umesh->boundary_type[(index)] = IS_BOUNDARY;

            // TODO: WE DON'T NEED ANYTHING SPECIAL HERE AS WE KNOW THE NORMALS
            // FROM THE CONSTRUCTION OF THE MESH, ALTHOUGH WE WILL NEED A
            // SUFFICIENT METHOD WHEN WE START USING MORE COMPLEX MESHES
            umesh->boundary_normal_x[(index)] =
                (kk == 0) ? -1.0 : ((kk == nx) ? 1.0 : 0.0);
            umesh->boundary_normal_y[(index)] =
                (jj == 0) ? -1.0 : ((jj == ny) ? 1.0 : 0.0);
            umesh->boundary_normal_z[(index)] =
                (ii == 0) ? -1.0 : ((ii == nz) ? 1.0 : 0.0);
          }
        } else {
          umesh->boundary_index[(node_index)] = IS_INTERIOR;
        }
      }
    }
  }

  // TODO: I really don't think that it is reasonable to include some of the
  // connectivity sets in this general unstructured functionality, but this will
  // need to be cleaned up later in order to make sure that the ALE code hal3d
  // can be completed.

  return allocated;
}
