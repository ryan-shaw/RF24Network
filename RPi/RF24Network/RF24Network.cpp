/*
 Copyright (C) 2011 James Coliz, Jr. <maniacbug@ymail.com>
 Copyright (C) 2014 Rei <devel@reixd.net>

 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 version 2 as published by the Free Software Foundation.
 */
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include "RF24Network_config.h"
#include <RF24/RF24.h>
#include "RF24Network.h"

uint16_t RF24NetworkHeader::next_id = 1;

uint64_t pipe_address( uint16_t node, uint8_t pipe );
#if defined (RF24NetworkMulticast)
  uint16_t levelToAddress( uint8_t level );
#endif
bool is_valid_address( uint16_t node );
uint32_t nFails = 0, nOK=0;

/******************************************************************/

RF24Network::RF24Network( RF24& _radio ): radio(_radio), frame_size(MAX_FRAME_SIZE)
{}

/******************************************************************/

void RF24Network::begin(uint8_t _channel, uint16_t _node_address ) {
  if (! is_valid_address(_node_address) ) {
    return;
  }

  node_address = _node_address;

  if ( ! radio.isValid() ) {
    return;
  }

  // Set up the radio the way we want it to look
  radio.setChannel(_channel);
  radio.setDataRate(RF24_1MBPS);
  radio.setCRCLength(RF24_CRC_16);
  radio.enableDynamicAck();
  radio.enableDynamicPayloads();

  //uint8_t retryVar = (node_address % 7) + 5;
  uint8_t retryVar = (((node_address % 6)+1) *2) + 3;
  radio.setRetries(retryVar, 5);
  txTimeout = 25;
  routeTimeout = txTimeout*9;

  // Setup our address helper cache
  setup_address();

  // Open up all listening pipes
  int i = 6;
  while (i--) {
    radio.openReadingPipe(i,pipe_address(_node_address,i));
  }
  #if defined (RF24NetworkMulticast)
    uint8_t count = 0; uint16_t addy = _node_address;
    while(addy) {
        addy/=8;
        count++;
    }
    multicast_level = count;
  #endif
  radio.startListening();

}

/******************************************************************/
void RF24Network::failures(uint32_t *_fails, uint32_t *_ok){
    *_fails = nFails;
    *_ok = nOK;
}

uint8_t RF24Network::update(void)
{
  // if there is data ready
  uint8_t pipe_num;
  while ( radio.isValid() && radio.available(&pipe_num) )
  {
    // Dump the payloads until we've gotten everything

    //while (radio.available())
    //{
      // Fetch the payload, and see if this was the last one.	  
      size_t len = radio.getDynamicPayloadSize();
	  if(len == 0){ /*printf("bad payload dropped\n");*/continue; }
      radio.read( frame_buffer, len );

      //Do we have a valid length for a frame?
      //We need at least a frame with header a no payload (payload_size equals 0).
      if (len < sizeof(RF24NetworkHeader))
        continue;

      // Read the beginning of the frame as the header
      RF24NetworkHeader header;
      memcpy(&header,frame_buffer,sizeof(RF24NetworkHeader));

      IF_SERIAL_DEBUG(printf_P("%u: MAC Received on %u %s\n\r",millis(),pipe_num,header.toString()));
      if (len) {
        IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: Rcv MAC frame size %i\n",millis(),len););
        IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: Rcv MAC frame ",millis()); const char* charPtr = reinterpret_cast<const char*>(frame_buffer); for (size_t i = 0; i < len; i++) { printf("%02X ", charPtr[i]); }; printf("\n\r"));
      }

      // Throw it away if it's not a valid address
      if ( !is_valid_address(header.to_node) ){
        continue;
      }

      IF_SERIAL_DEBUG(printf_P("%u: MAC Valid frame from %i with size %i received.\n\r",millis(),header.from_node,len));

      // Build the full frame
      RF24NetworkFrame frame = RF24NetworkFrame(header,frame_buffer+sizeof(RF24NetworkHeader),len-sizeof(RF24NetworkHeader));

      uint8_t res = header.type;
      // Is this for us?
      if ( header.to_node == node_address ){
            if(res == NETWORK_ACK){
                #ifdef SERIAL_DEBUG_ROUTING
                    printf_P(PSTR("MAC: Network ACK Rcvd\n"));
                #endif
                return NETWORK_ACK;
            }
            enqueue(frame);


      }else{

      #if defined   (RF24NetworkMulticast)
            if( header.to_node == 0100){
                if(header.id != lastMultiMessageID){
                    if(multicastRelay){
                        #ifdef SERIAL_DEBUG_ROUTING
                            printf_P(PSTR("MAC: FWD multicast frame from 0%o to level %d\n"),header.from_node,multicast_level+1);
                        #endif
                        write(levelToAddress(multicast_level)<<3,4);
                    }
                enqueue(frame);
                lastMultiMessageID = header.id;
                }
                #ifdef SERIAL_DEBUG_ROUTING
                else{
                    printf_P(PSTR("MAC: Drop duplicate multicast frame %u from 0%o\n"),header.id,header.from_node);
                }
                #endif
            }else{
                write(header.to_node,1);    //Send it on, indicate it is a routed payload
            }
        #else
        //if(radio.available()){printf("------FLUSHED DATA --------------");}
        write(header.to_node,1);    //Send it on, indicate it is a routed payload
        #endif
     }

      // NOT NEEDED anymore.  Now all reading pipes are open to start.
#if 0
      // If this was for us, from one of our children, but on our listening
      // pipe, it could mean that we are not listening to them.  If so, open up
      // and listen to their talking pipe

      if ( header.to_node == node_address && pipe_num == 0 && is_descendant(header.from_node) )
      {
        uint8_t pipe = pipe_to_descendant(header.from_node);
        radio.openReadingPipe(pipe,pipe_address(node_address,pipe));

        // Also need to open pipe 1 so the system can get the full 5-byte address of the pipe.
        radio.openReadingPipe(1,pipe_address(node_address,1));
      }
#endif
    //}
  }
  return 0;
}

/******************************************************************/

bool RF24Network::enqueue(RF24NetworkFrame frame) {
  bool result = false;

  if (frame.header.fragment_id > 1 && frame.header.type == NETWORK_MORE_FRAGMENTS) {
    //Set the more fragments flag to indicate a fragmented frame
    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: MAC fragmented payload of size %i Bytes with fragmentID '%i' received.\n\r",millis(),frame.message_size,frame.header.fragment_id););

    //Append payload
    appendFragmentToFrame(frame);
    result = true;

	} else if (frame.header.fragment_id == 1 && frame.header.type == NETWORK_LAST_FRAGMENT) {
    //Set the last fragment flag to indicate the last fragment
    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: MAC Last fragment with size %i Bytes and fragmentID '%i' received.\n\r",millis(),frame.message_size,frame.header.fragment_id););

    //Append payload
    appendFragmentToFrame(frame);	
	
    IF_SERIAL_DEBUG(printf_P(PSTR("%u: NET Enqueue assembled frame @%x "),millis(),frame_queue.size()));
    //Push the assembled frame in the frame_queue and remove it from cache
	frame_queue.push( frameFragmentsCache[ std::make_pair(frame.header.from_node,frame.header.id) ] );
    frameFragmentsCache.erase( std::make_pair(frame.header.from_node,frame.header.id) );

    result = true;

  } else {
    IF_SERIAL_DEBUG(printf_P(PSTR("%u: NET Enqueue @%x "),millis(),frame_queue.size()));

    // Copy the current frame into the frame queue
    frame_queue.push(frame);
    result = true;
  }

  if (result) {
    IF_SERIAL_DEBUG(printf("ok\n\r"));
  } else {
    IF_SERIAL_DEBUG(printf("failed\n\r"));
  }

  return result;
}

/******************************************************************/

void RF24Network::appendFragmentToFrame(RF24NetworkFrame frame) {

  if (frameFragmentsCache.count(std::make_pair(frame.header.from_node,frame.header.id)) == 0 ) {
	
	//If there is an un-finished fragment:
	 for (std::map<std::pair<uint16_t, uint16_t>, RF24NetworkFrame>::iterator it=frameFragmentsCache.begin(); it!=frameFragmentsCache.end(); ++it){		
		if( it->first.first == frame.header.from_node){
			frameFragmentsCache.erase( it );
			//printf("Map Size: %d\n",frameFragmentsCache.size());
			break;
		}
	 }

    //This is the first of many fragments
    frameFragmentsCache[ std::make_pair(frame.header.from_node,frame.header.id) ] = frame;
	
  } else {
    //We have at least received one fragments.
    //Append payload
    RF24NetworkFrame *f = &(frameFragmentsCache[ std::make_pair(frame.header.from_node,frame.header.id) ]);
	
	if(frame.message_size + f->message_size > MAX_PAYLOAD_SIZE){	
		frameFragmentsCache.erase( std::make_pair(frame.header.from_node,frame.header.id) );
		printf("cleared corrupt frame\n");
	}else{
    memcpy(f->message_buffer+f->message_size, frame.message_buffer, frame.message_size);
    //Increment message size
    f->message_size += frame.message_size;
    //Update header
    f->header = frame.header;
	}
  }
}

/******************************************************************/

bool RF24Network::available(void)
{
  // Are there frames on the queue for us?
  return (!frame_queue.empty());
}

/******************************************************************/

uint16_t RF24Network::parent() const
{
  if ( node_address == 0 )
    return -1;
  else
    return parent_node;
}

/******************************************************************/

void RF24Network::peek(RF24NetworkHeader& header)
{
  if ( available() )
  {
    RF24NetworkFrame frame = frame_queue.front();
    memcpy(&header,&frame,sizeof(RF24NetworkHeader));
  }
}

/******************************************************************/

size_t RF24Network::read(RF24NetworkHeader& header,void* message, size_t maxlen)
{
  size_t bufsize = 0;

  if ( available() )
  {
    RF24NetworkFrame frame = frame_queue.front();

    // How much buffer size should we actually copy?
    bufsize = std::min(frame.message_size,maxlen);

    memcpy(&header,&(frame.header),sizeof(RF24NetworkHeader));
    memcpy(message,frame.message_buffer,bufsize);

    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: NET message size %i\n",millis(),frame.message_size););
    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: NET message ",millis()); const char* charPtr = reinterpret_cast<const char*>(message); for (size_t i = 0; i < bufsize; i++) { printf("%02X ", charPtr[i]); }; printf("\n\r"));
    IF_SERIAL_DEBUG(printf_P(PSTR("%u: NET readed %s\n\r"),millis(),header.toString()));

    frame_queue.pop();
  }
  return bufsize;
}

/******************************************************************/
#if defined RF24NetworkMulticast

bool RF24Network::multicast(RF24NetworkHeader& header,const void* message, size_t len, uint8_t level){
  // Fill out the header

  header.to_node = 0100;
  header.from_node = node_address;

  // Build the full frame to send
  RF24NetworkFrame frame = RF24NetworkFrame(header,message,std::min(sizeof(message),len));

  IF_SERIAL_DEBUG(printf_P(PSTR("%u: NET Sending %s\n\r"),millis(),header.toString()));
  if (len)
  {
    IF_SERIAL_DEBUG(const uint16_t* i = reinterpret_cast<const uint16_t*>(message);printf_P(PSTR("%u: NET message %04x\n\r"),millis(),*i));
  }

  //uint16_t levelAddr = (level * 10)*8;
  uint16_t levelAddr = 1;
  levelAddr = levelAddr << ((level-1) * 3);

  return write(levelAddr,4);

}
#endif

/******************************************************************/
bool RF24Network::write(RF24NetworkHeader& header,const void* message, size_t len){
    return write(header,message,len,070);
}
/******************************************************************/
bool RF24Network::write(RF24NetworkHeader& header,const void* message, size_t len, uint16_t writeDirect){
  bool txSuccess = true;

  //Check payload size
  if (len > MAX_PAYLOAD_SIZE) {
    IF_SERIAL_DEBUG(printf("%u: NET write message failed. Given 'len' is bigger than the MAX Payload size of %i\n\r",millis(),MAX_PAYLOAD_SIZE););
    return false;
  }

  //If message payload length fits in a single message
  //then use the normal _write() function
  if (len <= max_frame_payload_size) {
    return _write(header,message,len,writeDirect);
  }

  //If the message payload is too big, whe cannot generate enough fragments
  //and enumerate them
  if (len > 255*max_frame_payload_size) {

    txSuccess = false;
    return txSuccess;
  }

  //The payload is smaller than MAX_PAYLOAD_SIZE and we can enumerate the fragments.
  // --> We cann transmit the message.

  //Divide the message payload into chuncks of max_frame_payload_size
  uint8_t fragment_id = 1 + ((len - 1) / max_frame_payload_size);  //the number of fragments to send = ceil(len/max_frame_payload_size)
  uint8_t msgCount = 0;

  IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: NET total message fragments %i\n\r",millis(),fragment_id););

  //Iterate over the payload chuncks
  //  Assemble a new message, copy and fill out the header
  //  Try to send this message
  //  If it fails
  //    then break
  //    return result as false
  while (fragment_id > 0) {

    //Copy and fill out the header
    RF24NetworkHeader fragmentHeader = header;
    fragmentHeader.fragment_id = fragment_id;

    if (fragment_id == 1) {
      fragmentHeader.type = NETWORK_LAST_FRAGMENT;  //Set the last fragment flag to indicate the last fragment
    } else {
      fragmentHeader.type = NETWORK_MORE_FRAGMENTS; //Set the more fragments flag to indicate a fragmented frame
    }

    size_t offset = msgCount*max_frame_payload_size;
    size_t fragmentLen = std::min(len-offset,max_frame_payload_size);

    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: NET try to transmit fragmented payload of size %i Bytes with fragmentID '%i'\n\r",millis(),fragmentLen,fragment_id););

    //Try to send the payload chunk with the copied header
    bool ok = _write(fragmentHeader,message+offset,fragmentLen,writeDirect);
    if (!ok) {
      IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: NET message transmission with fragmentID '%i' failed. Abort.\n\r",millis(),fragment_id););
      txSuccess = false;
      break;
    }
    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: NET message transmission with fragmentID '%i' sucessfull.\n\r",millis(),fragment_id););

    //Message was successful sent
    //Check and modify counters
    fragment_id--;
    msgCount++;
  }
  int frag_delay = int(len/16); delay( std::min(frag_delay,15)  ); 
  //Return true if all the chuncks where sent successfuly
  //else return false
  IF_SERIAL_DEBUG(printf("%u: NET total message fragments sent %i. txSuccess ",millis(),msgCount); printf("%s\n\r", txSuccess ? "YES" : "NO"););
  return txSuccess;
}
/******************************************************************/

bool RF24Network::_write(RF24NetworkHeader& header,const void* message, size_t len, uint16_t writeDirect)
{
  // Fill out the header
  header.from_node = node_address;

  // Build the full frame to send
  memcpy(frame_buffer,&header,sizeof(RF24NetworkHeader));
  frame_size = sizeof(RF24NetworkHeader); //Set the current frame size
  if (len) {
    memcpy(frame_buffer + sizeof(RF24NetworkHeader),message,std::min(MAX_FRAME_SIZE-sizeof(RF24NetworkHeader),len));
    frame_size += len; //Set the current frame size
  }

  IF_SERIAL_DEBUG(printf_P(PSTR("%u: NET Sending %s\n\r"),millis(),header.toString()));
  if (frame_size)
  {
   // IF_SERIAL_DEBUG(const uint16_t* i = reinterpret_cast<const uint16_t*>(message);printf_P(PSTR("%u: NET message %04x\n\r"),millis(),*i));
    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: MAC frame size %i\n",millis(),frame_size););
    IF_SERIAL_DEBUG_FRAGMENTATION(printf("%u: MAC frame ",millis()); const char* charPtr = reinterpret_cast<const char*>(frame_buffer); for (size_t i = 0; i < frame_size; i++) { printf("%02X ", charPtr[i]); }; printf("\n\r"));
  }


  // If the user is trying to send it to himself
  if ( header.to_node == node_address ){
    // Build the frame to send
    RF24NetworkFrame frame = RF24NetworkFrame(header,message,std::min(MAX_FRAME_SIZE-sizeof(RF24NetworkHeader),len));
    // Just queue it in the received queue
    return enqueue(frame);
  }else{
    if(writeDirect != 070){
        if(header.to_node == writeDirect){
            return write(writeDirect,2);
        }else{
            return write(writeDirect,3);
        }
    }else{
        // Otherwise send it out over the air
        return write(header.to_node,0);
    }
  }
}

/******************************************************************/

bool RF24Network::write(uint16_t to_node, uint8_t directTo)
{
  bool ok = false;
  bool multicast = 0; // Radio ACK requested = 0
  const uint16_t fromAddress = frame_buffer[0] | (frame_buffer[1] << 8);
  const uint16_t logicalAddress = frame_buffer[2] | (frame_buffer[3] << 8);

  // Throw it away if it's not a valid address
  if ( !is_valid_address(to_node) )
    return false;

  // First, stop listening so we can talk.
  //radio.stopListening();

  // Where do we send this?  By default, to our parent
  uint16_t send_node = parent_node;
  // On which pipe
  uint8_t send_pipe = parent_pipe%5;

 if(directTo>1){
    send_node = to_node;
    multicast = 1;
    if(directTo == 4){
        send_pipe=0;
    }
  }

  // If the node is a direct child,
  else if ( is_direct_child(to_node) )
  {
    // Send directly
    send_node = to_node;

    // To its listening pipe
    send_pipe = 5;
  }
  // If the node is a child of a child
  // talk on our child's listening pipe,
  // and let the direct child relay it.
  else if ( is_descendant(to_node) )
  {
    send_node = direct_child_route_to(to_node);
    send_pipe = 5;
  }


 // if( ( send_node != to_node) || frame_buffer[6] == NETWORK_ACK ){
   //     multicast = 1;
   //}

  IF_SERIAL_DEBUG(printf_P(PSTR("%u: MAC Sending to 0%o via 0%o on pipe %x\n\r"),millis(),logicalAddress,send_node,send_pipe));


  // Put the frame on the pipe
  ok = write_to_pipe( send_node, send_pipe, multicast );
  //printf("Multi %d\n",multicast);

  #if defined (SERIAL_DEBUG_ROUTING) || defined(SERIAL_DEBUG)
    if(!ok){ printf_P(PSTR("%u: MAC Send fail to 0%o from 0%o via 0%o on pipe %x\n\r"),millis(),logicalAddress,fromAddress,send_node,send_pipe); }
  #endif

       if( directTo == 1 && ok && send_node == to_node && frame_buffer[6] != NETWORK_ACK && fromAddress != node_address ){
            frame_buffer[6] = NETWORK_ACK;
            frame_buffer[2] = frame_buffer[0]; frame_buffer[3] = frame_buffer[1];
            write(fromAddress,1);
            #if defined (SERIAL_DEBUG_ROUTING)
                printf("MAC: Route OK to 0%o ACK sent to 0%o\n",to_node,fromAddress);
            #endif
       }



  // NOT NEEDED anymore.  Now all reading pipes are open to start.
#if 0
  // If we are talking on our talking pipe, it's possible that no one is listening.
  // If this fails, try sending it on our parent's listening pipe.  That will wake
  // it up, and next time it will listen to us.

  if ( !ok && send_node == parent_node )
    ok = write_to_pipe( parent_node, 0 );
#endif

  // Now, continue listening
  radio.startListening();

  if( ok && (send_node != logicalAddress) && (directTo==0 || directTo == 3 )){
        uint32_t reply_time = millis();
        while( update() != NETWORK_ACK){
            if(millis() - reply_time > routeTimeout){
                ok=0;
                #ifdef SERIAL_DEBUG_ROUTING
                    printf_P(PSTR("%u: MAC Network ACK fail from 0%o via 0%o on pipe %x\n\r"),millis(),logicalAddress,send_node,send_pipe);
                #endif
                break;
            }
        }
        //if(pOK){printf("pOK\n");}
   }
    if(ok == true){
            nOK++;
    }else{  nFails++;
    }
return ok;
}

/******************************************************************/

bool RF24Network::write_to_pipe( uint16_t node, uint8_t pipe, bool multicast )
{
  bool ok = false;

  uint64_t out_pipe = pipe_address( node, pipe );

  // First, stop listening so we can talk
  radio.stopListening();
  // Open the correct pipe for writing.
  radio.openWritingPipe(out_pipe);

  // Retry a few times
  radio.writeFast(frame_buffer, frame_size,multicast);
  ok = radio.txStandBy(txTimeout);
  //ok = radio.write(frame_buffer,frame_size);

  IF_SERIAL_DEBUG(printf_P(PSTR("%u: MAC Sent on %x %s\n\r"),millis(),(uint32_t)out_pipe,ok?PSTR("ok"):PSTR("failed")));

  return ok;
}

/******************************************************************/

const char* RF24NetworkHeader::toString(void) const
{
  static char buffer[45];
  //snprintf_P(buffer,sizeof(buffer),PSTR("id %04x from 0%o to 0%o type %c"),id,from_node,to_node,type);
  return buffer;
}

/******************************************************************/

bool RF24Network::is_direct_child( uint16_t node )
{
  bool result = false;

  // A direct child of ours has the same low numbers as us, and only
  // one higher number.
  //
  // e.g. node 0234 is a direct child of 034, and node 01234 is a
  // descendant but not a direct child

  // First, is it even a descendant?
  if ( is_descendant(node) )
  {
    // Does it only have ONE more level than us?
    uint16_t child_node_mask = ( ~ node_mask ) << 3;
    result = ( node & child_node_mask ) == 0 ;
  }

  return result;
}

/******************************************************************/

bool RF24Network::is_descendant( uint16_t node )
{
  return ( node & node_mask ) == node_address;
}

/******************************************************************/

void RF24Network::setup_address(void)
{
  // First, establish the node_mask
  uint16_t node_mask_check = 0xFFFF;
  while ( node_address & node_mask_check )
    node_mask_check <<= 3;

  node_mask = ~ node_mask_check;

  // parent mask is the next level down
  uint16_t parent_mask = node_mask >> 3;

  // parent node is the part IN the mask
  parent_node = node_address & parent_mask;

  // parent pipe is the part OUT of the mask
  uint16_t i = node_address;
  uint16_t m = parent_mask;
  while (m)
  {
    i >>= 3;
    m >>= 3;
  }
  parent_pipe = i;
  //parent_pipe=i-1;

#ifdef SERIAL_DEBUG
  printf_P(PSTR("setup_address node=0%o mask=0%o parent=0%o pipe=0%o\n\r"),node_address,node_mask,parent_node,parent_pipe);
#endif
}

/******************************************************************/

uint16_t RF24Network::direct_child_route_to( uint16_t node )
{
  // Presumes that this is in fact a child!!

  uint16_t child_mask = ( node_mask << 3 ) | 0B111;
  return node & child_mask ;
}

/******************************************************************/

uint8_t RF24Network::pipe_to_descendant( uint16_t node )
{
  uint16_t i = node;
  uint16_t m = node_mask;

  while (m)
  {
    i >>= 3;
    m >>= 3;
  }

  return i & 0B111;
}

/******************************************************************/

bool is_valid_address( uint16_t node )
{
  bool result = true;

  while(node)
  {
    uint8_t digit = node & 0B111;
    #if !defined (RF24NetworkMulticast)
    if (digit < 1 || digit > 5)
    #else
    if (digit < 0 || digit > 5) //Allow our out of range multicast address
    #endif
    {
      result = false;
      printf_P(PSTR("*** WARNING *** Invalid address 0%o\n\r"),node);
      break;
    }
    node >>= 3;
  }

  return result;
}

/******************************************************************/
#if defined (RF24NetworkMulticast)
void RF24Network::multicastLevel(uint8_t level){
  multicast_level = level;
  radio.stopListening();
  radio.openReadingPipe(0,pipe_address(levelToAddress(level),0));
  radio.startListening();
}

uint16_t levelToAddress(uint8_t level){
  uint16_t levelAddr = 1;
  levelAddr = levelAddr << ((level-1) * 3);
  return levelAddr;
}
#endif

/******************************************************************/

uint64_t pipe_address( uint16_t node, uint8_t pipe )
{

  static uint8_t address_translation[] = { 0xc3,0x3c,0x33,0xce,0x3e,0xe3,0xec };
  uint64_t result = 0xCCCCCCCCCCLL;
  uint8_t* out = reinterpret_cast<uint8_t*>(&result);

  // Translate the address to use our optimally chosen radio address bytes
    uint8_t count = 1; uint16_t dec = node;
  #if defined (RF24NetworkMulticast)
    if(pipe != 0 || !node){
  #endif
    while(dec){
        out[count]=address_translation[(dec % 8)];      // Convert our decimal values to octal, translate them to address bytes, and set our address
        dec /= 8;
        count++;
    }

    out[0] = address_translation[pipe];     // Set last byte by pipe number
  #if defined (RF24NetworkMulticast)
    }else{
        while(dec){
            dec/=8;
            count++;
        }
        out[1] = address_translation[count-1];
    }

  #endif

  IF_SERIAL_DEBUG(uint32_t* top = reinterpret_cast<uint32_t*>(out+1);printf_P(PSTR("%u: NET Pipe %i on node 0%o has address %x%x\n\r"),millis(),pipe,node,*top,*out));

  return result;
}


// vim:ai:cin:sts=2 sw=2 ft=cpp
