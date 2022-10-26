/**
 * @file aesd-circular-buffer.c
 * @brief Functions and data related to a circular buffer imlementation
 *
 * @author Dan Walkes
 * @date 2020-03-01
 * @copyright Copyright (c) 2020
 *
 */

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <string.h>
#endif

#include "aesd-circular-buffer.h"

/**
 * @param buffer the buffer to search for corresponding offset.  Any necessary locking must be performed by caller.
 * @param char_offset the position to search for in the buffer list, describing the zero referenced
 *      character index if all buffer strings were concatenated end to end
 * @param entry_offset_byte_rtn is a pointer specifying a location to store the byte of the returned aesd_buffer_entry
 *      buffptr member corresponding to char_offset.  This value is only set when a matching char_offset is found
 *      in aesd_buffer.
 * @return the struct aesd_buffer_entry structure representing the position described by char_offset, or
 * NULL if this position is not available in the buffer (not enough data is written).
 */
struct aesd_buffer_entry *aesd_circular_buffer_find_entry_offset_for_fpos(struct aesd_circular_buffer *buffer,
            size_t char_offset, size_t *entry_offset_byte_rtn )
{
    size_t count = 0;
    size_t current_size;
    size_t i = 0;
    
    // Starting from the output offet (the least recent thing entered into the buffer),
    // Loop through the whole buffer if it's full OR
    // If it's not full, loop until the output offset + the current loop count
    // = the input offset, with wrapping
    while(( buffer->full && (i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED)) || 
          (!buffer->full && !(((buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) == buffer->in_offs))) {
        
        // Find the size of the next string
        current_size = buffer->entry[(buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED].size;

        // check if the current sting contained the desired offeset and return if so
        count += current_size;
        if(count > char_offset){
            *entry_offset_byte_rtn = char_offset - (count - current_size);
            return &buffer->entry[(buffer->out_offs + i) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED];
        }

        i++;
    }

    return NULL;
}

/**
* Adds entry @param add_entry to @param buffer in the location specified in buffer->in_offs.
* If the buffer was already full, overwrites the oldest entry and advances buffer->out_offs to the
* new start location.
* Any necessary locking must be handled by the caller
* Any memory referenced in @param add_entry must be allocated by and/or must have a lifetime managed by the caller.
*/
const char *aesd_circular_buffer_add_entry(struct aesd_circular_buffer *buffer, const struct aesd_buffer_entry *add_entry)
{   
    const char *ret = NULL;

    // handle output increase and rollover if full
    if((buffer->in_offs == buffer->out_offs) && buffer->full){
        ret = buffer->entry[buffer->out_offs].buffptr;
        buffer->out_offs = (buffer->out_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
    }

    buffer->entry[buffer->in_offs] = *add_entry;

    // handle input increase and rollover if full
    buffer->in_offs = (buffer->in_offs + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    buffer->full = (buffer->in_offs == buffer->out_offs);

    return ret;
}

/**
* Initializes the circular buffer described by @param buffer to an empty struct
*/
void aesd_circular_buffer_init(struct aesd_circular_buffer *buffer)
{
    memset(buffer,0,sizeof(struct aesd_circular_buffer));
}
