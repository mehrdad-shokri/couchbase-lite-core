//
//  MessageBuilder.hh
//  blip_cpp
//
//  Created by Jens Alfke on 4/4/17.
//  Copyright © 2017 Couchbase. All rights reserved.
//

#pragma once
#include "Message.hh"
#include <sstream>

namespace litecore { namespace blip {

    /** A callback to provide data for an outgoing message. When called, it should copy data
        to the location in the `buf` parameter, with a maximum length of `capacity`. It should
        return the number of bytes written, or 0 on EOF, or a negative number on error. */
    using MessageDataSource = std::function<int(void* buf, size_t capacity)>;

    /** A temporary object used to construct an outgoing message (request or response).
        The message is sent by calling Connection::sendRequest() or MessageIn::respond(). */
    class MessageBuilder {
    public:
        using slice = fleece::slice;
        using alloc_slice = fleece::alloc_slice;

        typedef std::pair<slice, slice> property;

        /** Constructs a MessageBuilder for a request, optionally setting its Profile property. */
        MessageBuilder(slice profile = fleece::nullslice);

        /** Constructs a MessageBuilder for a request, with a list of properties. */
        MessageBuilder(std::initializer_list<property>);

        /** Constructs a MessageBuilder for a response. */
        MessageBuilder(MessageIn *inReplyTo);

        /** Adds a property. */
        MessageBuilder& addProperty(slice name, slice value);

        /** Adds a property with an integer value. */
        MessageBuilder& addProperty(slice name, int64_t value);

        /** Adds multiple properties. */
        MessageBuilder& addProperties(std::initializer_list<property>);

        struct propertySetter {
            MessageBuilder &builder;
            slice name;
            MessageBuilder& operator= (slice value)   {return builder.addProperty(name, value);}
            MessageBuilder& operator= (int64_t value) {return builder.addProperty(name, value);}
        };
        propertySetter operator[] (slice name)        { return {*this, name}; }

        /** Makes a response an error. */
        void makeError(Error);

        /** JSON encoder that can be used to write JSON to the body. */
        fleeceapi::JSONEncoder& jsonBody()          {finishProperties(); return _out;}

        /** Adds data to the body of the message. No more properties can be added afterwards. */
        MessageBuilder& write(slice s);
        MessageBuilder& operator<< (slice s)        {return write(s);}

        /** Clears the MessageBuilder so it can be used to create another message. */
        void reset();

        /** Callback to provide the body of the message; will be called whenever data is needed. */
        MessageDataSource dataSource;

        /** Callback to be invoked as the message is delivered (and replied to, if appropriate) */
        MessageProgressCallback onProgress;

        /** Is the message urgent (will be sent more quickly)? */
        bool urgent         {false};

        /** Should the message's body be gzipped? */
        bool compressed     {false};

        /** Should the message refuse replies? */
        bool noreply        {false};

        static slice untokenizeProperty(slice property);
        static uint8_t tokenizeProperty(slice property);

    protected:
        friend class MessageIn;
        friend class MessageOut;

        FrameFlags flags() const;
        alloc_slice extractOutput();
        void writeTokenizedString(std::ostream &out, slice str);

        MessageType type {kRequestType};

    private:
        void finishProperties();

        fleeceapi::JSONEncoder _out;    // Actually using it for the entire msg, not just JSON
        std::stringstream _properties;  // Accumulates encoded properties
        bool _wroteProperties {false};  // Have _properties been written to _out yet?
        uint32_t _propertiesLength {0};
    };

} }
